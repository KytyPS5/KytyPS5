// Live PerVertex execution with GPU compute unpack and native C++ pipeline.
void RenderExecutor::ExecutePerVertexPrototype(uint64_t submit_id, CommandBuffer& buffer,
                                               const DrawCallInfo& draw, DrawRenderState& state,
                                               vk::PrimitiveTopology        topology,
                                               const DrawEmitInfo&          emit,
                                               const DrawIndexBufferSource& index_source,
                                               bool primitive_restart_enable) {
	const auto& vs       = state.vertex_info[0];
	const auto* programs = GetPerVertexPrototypePrograms(
	    m_context.GetGraphics(), state.programs, vs, m_context.GetPipelineCache().DriverCache());
	EXIT_IF(programs == nullptr || !state.ps_active ||
	        topology != vk::PrimitiveTopology::eTriangleList || primitive_restart_enable ||
	        draw.index_count % 3 != 0);
	EXIT_IF(vs.logical_stage != ShaderType::Vertex || vs.buffers_num != 1 ||
	        vs.resources_num == 0 || vs.resources_num > 8 || HasShaderBufferWrites(vs.stage));
	const auto&    input     = vs.buffers[0];
	const uint32_t num_attrs = vs.resources_num;
	EXIT_IF(input.stride == 0 || input.fetch_index != 0 ||
	        input.attr_num != static_cast<int>(num_attrs) || input.addr == 0);

	for (uint32_t i = 0; i < num_attrs; ++i) {
		EXIT_IF(input.attr_indices[i] != static_cast<int>(i));
		const auto& descriptor = vs.resources[i];
		const auto  format     = ShaderRecompiler::Format::GetFormatInfo(descriptor.Format());
		EXIT_IF(format.byte_size > input.stride ||
		        input.attr_offsets[i] > input.stride - format.byte_size ||
		        descriptor.SwizzleEnabled() || descriptor.AddTid());
		std::array<uint8_t, 16> bytes {};
		std::array<uint32_t, 4> decoded {};
		EXIT_IF(!DecodePerVertexPrototypeAttribute(descriptor.Format(), bytes, decoded));
	}

	const uint64_t count64 = static_cast<uint64_t>(draw.index_count) * draw.instance_count;
	EXIT_IF(count64 == 0 || count64 > 65535);
	const auto count     = static_cast<uint32_t>(count64);
	auto&      graphics  = m_context.GetGraphics();
	auto&      scheduler = m_context.GetCommandScheduler();
	auto&      cache     = m_context.GetBufferCache();
	scheduler.EndRendering();

	const auto alignment = graphics.StorageMinAlignment();

	// Obtain vertex buffer from GPU cache without host readback
	const uint64_t vtx_total_bytes    = static_cast<uint64_t>(input.stride) * input.num_records;
	auto [vtx_buf_ptr, vtx_offset]    = cache.ObtainBuffer(input.addr, vtx_total_bytes, false);
	const uint64_t vtx_aligned_offset = Common::AlignDown(vtx_offset, alignment);
	const uint32_t vertex_byte_adjust = static_cast<uint32_t>(vtx_offset - vtx_aligned_offset);

	vk::Buffer idx_vk_buf         = nullptr;
	uint64_t   idx_aligned_offset = 0;
	uint32_t   index_byte_adjust  = 0;
	uint64_t   idx_buf_available  = 0;
	const uint32_t index_element_size =
	    index_source.host_data != nullptr ? sizeof(uint16_t) : index_source.guest_element_size;
	if (draw.IsIndexed()) {
		EXIT_IF(index_element_size != 1 && index_element_size != 2 && index_element_size != 4);
		const uint64_t idx_total_bytes = index_source.size;
		if (index_source.host_data != nullptr) {
			auto&      stream  = cache.GetUtilityBuffer(MemoryUsage::Stream);
			const auto offset  = stream.Copy(index_source.host_data, idx_total_bytes, 16);
			idx_vk_buf         = stream.Handle();
			idx_aligned_offset = Common::AlignDown(offset, alignment);
			index_byte_adjust  = static_cast<uint32_t>(offset - idx_aligned_offset);
			idx_buf_available =
			    stream.Size() > idx_aligned_offset ? (stream.Size() - idx_aligned_offset) : 0;
		} else {
			auto [idx_buf_ptr, offset] =
			    cache.ObtainBuffer(index_source.address, idx_total_bytes, false);
			idx_vk_buf         = idx_buf_ptr->Handle();
			idx_aligned_offset = Common::AlignDown(offset, alignment);
			index_byte_adjust  = static_cast<uint32_t>(offset - idx_aligned_offset);
			idx_buf_available  = idx_buf_ptr->Size() > idx_aligned_offset
			                         ? (idx_buf_ptr->Size() - idx_aligned_offset)
			                         : 0;
		}
	} else {
		idx_vk_buf         = vtx_buf_ptr->Handle();
		idx_aligned_offset = 0;
		index_byte_adjust  = 0;
		idx_buf_available  = vtx_buf_ptr->Size();
	}

	PreparedBindings  vertex_bindings = PrepareBindings(vs.stage);
	PreparedBindings  pixel_bindings  = PrepareBindings(state.ps_input_info.stage);
	PreparedBindings* stages[] {&vertex_bindings, &pixel_bindings};
	PrepareGraphicsBindings(stages, {state.color_info, state.color_count});

	// GPU-side intermediate buffers for unpack and capture
	auto id_buffer =
	    std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, count * 8);
	auto attribute_buffer = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                                 AllFlags, count * num_attrs * 16);
	auto captured = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags,
	                                         count64 * programs->layout.record_stride_vec4 * 16);

	const auto* unpack_pipeline =
	    GetPerVertexUnpackPipeline(graphics, m_context.GetPipelineCache().DriverCache());

	// Descriptor pool for unpack set (4 storage buffers) + extra set (3 storage buffers)
	vk::DescriptorPool           pool;
	const vk::DescriptorPoolSize pool_size(vk::DescriptorType::eStorageBuffer, 7);
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.maxSets       = 2;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	EXIT_IF(graphics.device.createDescriptorPool(&pool_info, nullptr, &pool) !=
	        vk::Result::eSuccess);

	// Allocate and update unpack descriptor set
	vk::DescriptorSet             unpack_set;
	vk::DescriptorSetAllocateInfo unpack_alloc {};
	unpack_alloc.descriptorPool     = pool;
	unpack_alloc.descriptorSetCount = 1;
	unpack_alloc.pSetLayouts        = &unpack_pipeline->set_layout;
	EXIT_IF(graphics.device.allocateDescriptorSets(&unpack_alloc, &unpack_set) !=
	        vk::Result::eSuccess);

	const uint64_t idx_bytes_raw =
	    draw.IsIndexed()
	        ? (index_source.size + index_byte_adjust)
	        : (vtx_total_bytes + vertex_byte_adjust);
	const uint64_t idx_size_bounded =
	    std::min(Common::AlignUp(idx_bytes_raw, 4), idx_buf_available);

	const uint64_t vtx_bytes_raw = vtx_total_bytes + vertex_byte_adjust;
	const uint64_t vtx_buf_available =
	    vtx_buf_ptr->Size() > vtx_aligned_offset ? (vtx_buf_ptr->Size() - vtx_aligned_offset) : 0;
	const uint64_t vtx_size_bounded =
	    std::min(Common::AlignUp(vtx_bytes_raw, 4) + 4, vtx_buf_available);

	const vk::DescriptorBufferInfo unpack_data[] {
	    {idx_vk_buf, idx_aligned_offset, idx_size_bounded},
	    {vtx_buf_ptr->Handle(), vtx_aligned_offset, vtx_size_bounded},
	    {id_buffer->Handle(), 0, id_buffer->Size()},
	    {attribute_buffer->Handle(), 0, attribute_buffer->Size()}};
	std::array<vk::WriteDescriptorSet, 4> unpack_writes;
	for (uint32_t i = 0; i < 4; ++i) {
		unpack_writes[i].dstSet          = unpack_set;
		unpack_writes[i].dstBinding      = i;
		unpack_writes[i].descriptorCount = 1;
		unpack_writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		unpack_writes[i].pBufferInfo     = &unpack_data[i];
	}
	graphics.device.updateDescriptorSets(4, unpack_writes.data(), 0, nullptr);

	// Allocate and update extra descriptor set (shared between capture compute & graphics replay)
	vk::DescriptorSet             extra_set;
	vk::DescriptorSetAllocateInfo extra_alloc {};
	extra_alloc.descriptorPool     = pool;
	extra_alloc.descriptorSetCount = 1;
	extra_alloc.pSetLayouts        = &programs->extra_layout;
	EXIT_IF(graphics.device.allocateDescriptorSets(&extra_alloc, &extra_set) !=
	        vk::Result::eSuccess);

	const vk::DescriptorBufferInfo extra_data[] {
	    {attribute_buffer->Handle(), 0, attribute_buffer->Size()},
	    {captured->Handle(), 0, captured->Size()},
	    {id_buffer->Handle(), 0, id_buffer->Size()}};
	std::array<vk::WriteDescriptorSet, 3> extra_writes;
	for (uint32_t i = 0; i < 3; ++i) {
		extra_writes[i].dstSet          = extra_set;
		extra_writes[i].dstBinding      = i;
		extra_writes[i].descriptorCount = 1;
		extra_writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		extra_writes[i].pBufferInfo     = &extra_data[i];
	}
	graphics.device.updateDescriptorSets(3, extra_writes.data(), 0, nullptr);

	// Prepare Unpack push constants
	UnpackPushConstants unpack_push {};
	unpack_push.total_invocations = count;
	unpack_push.index_count       = draw.index_count;
	unpack_push.first_vertex      = emit.first_vertex;
	unpack_push.vertex_offset     = emit.vertex_offset;
	unpack_push.first_instance    = emit.first_instance;
	unpack_push.vertex_stride     = input.stride;
	unpack_push.num_records       = input.num_records;

	uint32_t idx_flag = 0;
	if (draw.IsIndexed()) {
		idx_flag = 1u;
		if (index_element_size == 2) {
			idx_flag |= (1u << 1u);
		} else if (index_element_size == 4) {
			idx_flag |= (2u << 1u);
		}
	}
	unpack_push.packed_flags =
	    (index_byte_adjust & 0xffffu) | (idx_flag << 16u) | ((num_attrs & 0x0fu) << 19u);

	for (uint32_t i = 0; i < num_attrs; ++i) {
		const auto&    descriptor  = vs.resources[i];
		const auto     format_info = ShaderRecompiler::Format::GetFormatInfo(descriptor.Format());
		const uint32_t byte_offset = vertex_byte_adjust + input.attr_offsets[i];
		uint32_t format_kind = 0;
		if (descriptor.Format() == Prospero::BufferFormat::k16_16_16_16Float) {
			format_kind = 3;
		} else if (descriptor.Format() == Prospero::BufferFormat::k16_16_16_16SNorm) {
			format_kind = 2;
		} else if (format_info.type == ShaderRecompiler::Format::ComponentType::Float) {
			format_kind = 1;
		}
		const uint32_t comp_count = format_info.component_count;
		unpack_push.attrs[i].meta =
		    (byte_offset & 0xffffu) | (comp_count << 17u) | (format_kind << 20u);

		uint32_t bit_counts  = 0;
		uint32_t bit_offsets = 0;
		for (uint32_t c = 0; c < 4; ++c) {
			bit_counts |= (format_info.component_bits[c] & 0xffu) << (c * 8u);
			bit_offsets |= (format_info.component_bit_offset[c] & 0xffu) << (c * 8u);
		}
		unpack_push.attrs[i].bit_counts  = bit_counts;
		unpack_push.attrs[i].bit_offsets = bit_offsets;
	}

	auto cmd = buffer.Handle();

	// Step 1: GPU unpack compute pass
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, unpack_pipeline->pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, unpack_pipeline->pipeline_layout, 0, 1,
	                       &unpack_set, 0, nullptr);
	cmd.pushConstants(unpack_pipeline->pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                  sizeof(unpack_push), &unpack_push);
	cmd.dispatch((count + 63) / 64, 1, 1);

	// Barrier: Unpack -> Capture compute
	vk::MemoryBarrier unpack_barrier {};
	unpack_barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	unpack_barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &unpack_barrier, 0,
	                    nullptr, 0, nullptr);

	// Step 2: Capture compute pass
	PreparedBindings* vertex_stage[] {&vertex_bindings};
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, programs->capture_pipeline,
	               vertex_stage);
	cmd = buffer.Handle();
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                       programs->capture_pipeline.pipeline_layout, 1, 1, &extra_set, 0,
	                       nullptr);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, programs->capture_pipeline.pipeline);
	cmd.dispatch(count, 1, 1);

	// Barrier: Capture compute -> Graphics replay
	vk::MemoryBarrier capture_barrier {};
	capture_barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	capture_barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                    vk::PipelineStageFlagBits::eVertexShader |
	                        vk::PipelineStageFlagBits::eFragmentShader,
	                    {}, 1, &capture_barrier, 0, nullptr, 0, nullptr);

	// Step 3: Graphics replay pass
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    {state.color_info, state.color_count}, state.depth_info, {state.vertex_info.data(), 1},
	    buffer, &state.ps_input_info, topology, false, programs->graphics);
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         std::optional<PreparedBindings> {pixel_bindings});
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	cmd = buffer.Handle();
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline_layout, 1, 1,
	                       &extra_set, 0, nullptr);
	SetGraphicsDynamicParams(buffer, cmd, vs, state.depth_info, rendering);
	if (graphics.attachment_feedback_loop_enabled)
		cmd.setAttachmentFeedbackLoopEnableEXT(
		    rendering.depth_stencil_attachment.image_layout ==
		            vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
		        ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth}
		        : vk::ImageAspectFlags {});
	scheduler.BeginRendering(rendering);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	cmd.draw(count, 1, 0, 0);
	scheduler.EndRendering();
	if (HasShaderBufferWrites(state.ps_input_info.stage))
		ShaderWriteBarrier(cmd, vk::PipelineStageFlagBits::eFragmentShader);

	static uint64_t draw_counter = 0;
	draw_counter++;
	LOGF("PerVertexPrototype: recorded draw submission=%llu vertices=%u instances=%u "
	     "capture_bytes=%llu layout_stride=%u\n",
	     static_cast<unsigned long long>(draw_counter), count, draw.instance_count,
	     static_cast<unsigned long long>(captured->Size()), programs->layout.record_stride_vec4);

	// Asynchronous deferred reclamation: no CPU flush-and-wait stalls
	scheduler.DeferOperation([attr = std::move(attribute_buffer), id = std::move(id_buffer),
	                          cap = std::move(captured), pool, dev = graphics.device]() mutable {
		attr.reset();
		id.reset();
		cap.reset();
		dev.destroyDescriptorPool(pool);
	});
}
