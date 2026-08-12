#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/pipeline/shaderSubgroup.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
static void TraceComputeImageAddressAccess(const ShaderComputeInputInfo& input,
                                           const DescriptorCache::PreparedBindings& bindings,
                                           uint64_t shader_address, uint64_t submit_id,
                                           uint32_t frame_num,
                                           uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                           uint32_t mode) {
	const char* const address_text = std::getenv("KYTY_COMPUTE_IMAGE_HISTORY_ADDRESS");
	const char* const file_text    = std::getenv("KYTY_COMPUTE_IMAGE_HISTORY_FILE");
	if (address_text == nullptr || address_text[0] == '\0' || file_text == nullptr ||
	    file_text[0] == '\0') {
		return;
	}
	const bool wildcard = std::strcmp(address_text, "*") == 0;
	char*      end      = nullptr;
	const auto target   = wildcard ? 0u : std::strtoull(address_text, &end, 0);
	if (!wildcard && (end == address_text || *end != '\0')) {
		return;
	}
	const char* const width_text  = std::getenv("KYTY_COMPUTE_IMAGE_HISTORY_WIDTH");
	const char* const height_text = std::getenv("KYTY_COMPUTE_IMAGE_HISTORY_HEIGHT");
	const auto width_filter = width_text != nullptr ? std::strtoul(width_text, nullptr, 10) : 0u;
	const auto height_filter = height_text != nullptr ? std::strtoul(height_text, nullptr, 10) : 0u;

	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	std::vector<std::string> matches;
	auto append_image = [&](const char* binding_kind, const std::string& resource_text,
	                        const ShaderRecompiler::IR::ImageResource& image,
	                        const DescriptorCache::TextureBinding& binding) {
		const auto& info   = binding.desc.info;
		const auto  address = info.data.address;
		const auto  width   = info.extent.width;
		const auto  height  = info.extent.height;
		if ((!wildcard && address != target) ||
		    (width_filter != 0 && width != width_filter) ||
		    (height_filter != 0 && height != height_filter)) {
			return;
		}
		matches.push_back(fmt::format(
		    "{}\t{}\t{}\t{}\t{}\t{:012x}\t{}\t{}\t{}\t{}\t{}\t{}", binding_kind,
		    resource_text,
		    image.written ? "write" : "read", static_cast<uint32_t>(image.kind), image.source,
		    address, static_cast<uint32_t>(info.guest_format), width, height, info.extent.depth,
		    static_cast<uint32_t>(info.tile_mode),
		    static_cast<uint32_t>(info.type)));
	};
	for (uint32_t i = 0; i < bindings.resources.images.size() && i < program.info.images.size(); i++) {
		append_image("image", std::to_string(i), program.info.images[i],
		             bindings.resources.images[i]);
		if (i < bindings.resources.image_tables.size()) {
			for (uint32_t entry = 0; entry < bindings.resources.image_tables[i].size(); entry++) {
				append_image("image_table", fmt::format("{}.{}", i, entry),
				             program.info.images[i], bindings.resources.image_tables[i][entry]);
			}
		}
	}
	for (uint32_t i = 0; !wildcard && i < resources.buffers.size() &&
	                     i < program.info.buffers.size(); i++) {
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		if (descriptor.Base48() != target) {
			continue;
		}
		const auto& buffer = program.info.buffers[i];
		matches.push_back(fmt::format("buffer\t{}\t{}\t0\t{}\t{:012x}\t{}\t{}\t{}\t0\t0\t{}", i,
		                              buffer.written ? "write" : "read", buffer.source,
		                              descriptor.Base48(),
		                              static_cast<uint32_t>(descriptor.Format()), descriptor.Stride(),
		                              descriptor.NumRecords(),
		                              static_cast<uint32_t>(descriptor.Type())));
	}
	if (matches.empty()) {
		return;
	}

	const auto path = std::filesystem::path(file_text);
	Common::File::CreateDirectories(path.parent_path());
	const bool write_header = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
	std::ofstream file(path, std::ios::out | std::ios::app);
	if (!file) {
		LOGF("Compute image history: cannot open %s\n", Common::PathToString(path).c_str());
		return;
	}
	if (write_header) {
		file << "frame\tsubmit\tshader_hash\tshader_address\tgroups_x\tgroups_y\tgroups_z\t"
		        "local_x\tlocal_y\tlocal_z\tmode\tkind\tresource\taccess\tresource_kind\t"
		        "source\tguest_address\tformat\twidth_or_stride\theight_or_records\tdepth\t"
		        "tile\ttype\n";
	}
	for (const auto& match: matches) {
		file << frame_num << '\t' << submit_id << '\t' << fmt::format("{:016x}", program.shader_hash)
		     << '\t' << fmt::format("{:016x}", shader_address) << '\t' << group_x << '\t'
		     << group_y << '\t' << group_z << '\t' << input.threads_num[0] << '\t'
		     << input.threads_num[1] << '\t' << input.threads_num[2] << '\t'
		     << fmt::format("{:08x}", mode) << '\t' << match << '\n';
	}
}

static bool ComputeResourceDumpHashRequested(uint64_t shader_hash) {
	const char* current = std::getenv("KYTY_COMPUTE_RESOURCE_DUMP_HASHES");
	if (current == nullptr || current[0] == '\0') {
		return false;
	}
	while (*current != '\0') {
		while (*current == ' ' || *current == '\t' || *current == ',' || *current == ';') {
			current++;
		}
		if (*current == '\0') {
			break;
		}
		char*      end   = nullptr;
		const auto value = std::strtoull(current, &end, 0);
		if (end == current) {
			return false;
		}
		if (value == shader_hash) {
			return true;
		}
		current = end;
	}
	return false;
}

static void DumpDescriptorValue(Common::File& file, const char* kind, uint32_t index,
                                const ShaderRecompiler::IR::DescriptorValue& value) {
	file.Printf("%s[%u] dword_count=%u", kind, index, value.dword_count);
	for (uint32_t i = 0; i < value.dword_count; i++) {
		file.Printf(" d%u=%08" PRIx32, i, value.dwords[i]);
	}
	file.Printf("\n");
}

static void DumpComputeResourceSnapshotOnce(const ShaderComputeInputInfo& input,
                                            uint64_t shader_address, uint32_t frame_num,
                                            uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                            uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto  hash      = program.shader_hash;
	const char* directory = std::getenv("KYTY_COMPUTE_RESOURCE_DUMP_DIR");
	if (directory == nullptr || directory[0] == '\0' || !ComputeResourceDumpHashRequested(hash)) {
		return;
	}

	// DispatchDirect holds the renderer context mutex, so this process-lifetime set is serialized.
	static std::vector<uint64_t> dumped_hashes;
	if (std::find(dumped_hashes.begin(), dumped_hashes.end(), hash) != dumped_hashes.end()) {
		return;
	}
	dumped_hashes.push_back(hash);

	const auto path =
	    std::filesystem::path(directory) / fmt::format("compute-resources-{:016x}.txt", hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file;
	if (!file.Create(path)) {
		LOGF("Compute resource dump: cannot create %s\n", Common::PathToString(path).c_str());
		return;
	}

	file.Printf("shader_hash=%016" PRIx64 " shader_address=%016" PRIx64
	            " frame=%u groups=%u,%u,%u mode=%08" PRIx32 "\n",
	            hash, shader_address, frame_num, group_x, group_y, group_z, mode);
	file.Printf("local=%u,%u,%u wave_size=%u lds_dwords=%u buffers=%zu images=%zu "
	            "samplers=%zu addresses=%zu\n",
	            input.threads_num[0], input.threads_num[1], input.threads_num[2], input.wave_size,
	            input.lds_size_dwords, resources.buffers.size(), resources.images.size(),
	            resources.samplers.size(), resources.addresses.size());

	for (uint32_t i = 0; i < resources.buffers.size(); i++) {
		DumpDescriptorValue(file, "buffer", i, resources.buffers[i]);
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		file.Printf("  decoded base=%012" PRIx64
		            " stride=%u records=%u format=%u dst_sel=%u,%u,%u,%u swizzle=%u "
		            "index_stride=%u add_tid=%u oob=%u type=%u\n",
		            descriptor.Base48(), descriptor.Stride(), descriptor.NumRecords(),
		            static_cast<uint32_t>(descriptor.Format()), descriptor.DstSelX(),
		            descriptor.DstSelY(),
		            descriptor.DstSelZ(), descriptor.DstSelW(),
		            descriptor.SwizzleEnabled() ? 1u : 0u, descriptor.IndexStride(),
		            descriptor.AddTid() ? 1u : 0u, descriptor.OutOfBounds(),
		            static_cast<uint32_t>(descriptor.Type()));
	}
	for (uint32_t i = 0; i < resources.images.size(); i++) {
		DumpDescriptorValue(file, "image", i, resources.images[i]);
		const auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
		file.Printf("  decoded base=%010" PRIx64
		            " format=%u extent=%u,%u,%u dst_sel=%u,%u,%u,%u base_level=%u "
		            "last_level=%u max_mip=%u tile=%u type=%u meta=%016" PRIx64 "\n",
		            descriptor.Base40(), static_cast<uint32_t>(descriptor.Format()),
		            static_cast<uint32_t>(descriptor.Width5()) + 1u,
		            static_cast<uint32_t>(descriptor.Height5()) + 1u,
		            static_cast<uint32_t>(descriptor.Depth()) + 1u, descriptor.DstSelX(),
		            descriptor.DstSelY(), descriptor.DstSelZ(), descriptor.DstSelW(),
		            descriptor.BaseLevel(), descriptor.LastLevel(), descriptor.MaxMip(),
		            static_cast<uint32_t>(descriptor.TileMode()),
		            static_cast<uint32_t>(descriptor.Type()), descriptor.MetaAddr());
	}
	for (uint32_t i = 0; i < resources.samplers.size(); i++) {
		DumpDescriptorValue(file, "sampler", i, resources.samplers[i]);
		const auto descriptor =
		    DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[i]);
		file.Printf("  decoded clamp=%u,%u,%u filter=%u,%u,%u mip=%u lod=%u,%u bias=%u "
		            "force_unorm=%u compare=%u\n",
		            descriptor.ClampX(), descriptor.ClampY(), descriptor.ClampZ(),
		            descriptor.XyMagFilter(), descriptor.XyMinFilter(), descriptor.ZFilter(),
		            descriptor.MipFilter(), descriptor.MinLod(), descriptor.MaxLod(),
		            descriptor.LodBias(), descriptor.ForceUnormCoords() ? 1u : 0u,
		            descriptor.DepthCompareFunc());
	}
	for (uint32_t i = 0; i < resources.addresses.size(); i++) {
		file.Printf("address[%u] guest_base=%016" PRIx64 " binding_base=%016" PRIx64 "\n", i,
		            resources.addresses[i].guest_base, resources.addresses[i].binding_base);
	}
	file.Printf("user_data count=%zu", resources.user_data.size());
	for (uint32_t i = 0; i < resources.user_data.size(); i++) {
		file.Printf(" u%u=%08" PRIx32, i, resources.user_data[i]);
	}
	file.Printf("\nflattened_srt count=%zu", resources.flattened_srt.size());
	for (uint32_t i = 0; i < resources.flattened_srt.size(); i++) {
		file.Printf(" s%u=%08" PRIx32, i, resources.flattened_srt[i]);
	}
	file.Printf("\n");
	file.Close();
	LOGF("Compute resource dump: wrote %s\n", Common::PathToString(path).c_str());
}

static bool ComputeOutputDumpHashRequested(uint64_t shader_hash) {
	const char* const text = std::getenv("KYTY_COMPUTE_OUTPUT_DUMP_HASH");
	if (text == nullptr || text[0] == '\0') {
		return false;
	}
	const char* cursor = text;
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		char*      end   = nullptr;
		const auto value = std::strtoull(cursor, &end, 0);
		if (end == cursor) {
			return false;
		}
		if (value == shader_hash) {
			return true;
		}
		cursor = end;
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}
		if (*cursor != '\0' && *cursor != ',') {
			return false;
		}
	}
	return false;
}

static bool ConsumeComputeOutputDumpOccurrence(uint64_t shader_hash) {
	if (!ComputeOutputDumpHashRequested(shader_hash)) {
		return false;
	}
	const char* const text = std::getenv("KYTY_COMPUTE_OUTPUT_DUMP_OCCURRENCE");
	const auto requested = text != nullptr ? std::max<uint64_t>(std::strtoull(text, nullptr, 10), 1u)
	                                       : 1u;
	// DispatchDirect holds the renderer context mutex, so this counter is serialized.
	static std::unordered_map<uint64_t, uint64_t> occurrences;
	return ++occurrences[shader_hash] == requested;
}

static uint32_t ComputeOutputDumpResource(const ShaderRecompiler::IR::Program& program) {
	const char* const text = std::getenv("KYTY_COMPUTE_OUTPUT_DUMP_RESOURCE");
	if (text != nullptr && text[0] != '\0') {
		return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (program.info.images[i].written) {
			return i;
		}
	}
	return 0u;
}

static void WriteComputeOutputDump(const ShaderRecompiler::IR::Program& program,
	                               const DescriptorCache::PreparedBindings& bindings,
	                               RenderContext& context,
	                               const char* directory_override = nullptr,
	                               uint32_t resource_override = UINT32_MAX,
	                               const char* file_prefix = "compute-output") {
	const char* const directory = directory_override != nullptr
	                                  ? directory_override
	                                  : std::getenv("KYTY_COMPUTE_OUTPUT_DUMP_DIR");
	if (directory == nullptr || directory[0] == '\0') {
		return;
	}
	const auto resource = resource_override != UINT32_MAX
	                          ? resource_override
	                          : ComputeOutputDumpResource(program);
	if (resource >= bindings.resources.images.size()) {
		LOGF("Compute output dump: shader=0x%016" PRIx64 " resource=%u exceeds images=%zu\n",
		     program.shader_hash, resource, bindings.resources.images.size());
		return;
	}

	auto& cache = context.GetTextureCache();
	auto& image = cache.GetImage(bindings.resources.images[resource].image_id);
	const uint32_t width  = image.info.extent.width;
	const uint32_t height = image.info.extent.height;
	const uint64_t bytes  = static_cast<uint64_t>(width) * height * image.info.bytes_per_block;
	if (bytes == 0 || bytes > static_cast<uint64_t>(UINT32_MAX)) {
		LOGF("Compute output dump: invalid size shader=0x%016" PRIx64
		     " resource=%u extent=%ux%u bpb=%u\n",
		     program.shader_hash, resource, width, height, image.info.bytes_per_block);
		return;
	}

	VulkanBuffer staging {};
	staging.usage = vk::BufferUsageFlagBits::eTransferDst;
	staging.memory.property =
	    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	context.GetGraphics().CreateBuffer(bytes, staging);
	if (staging.buffer == nullptr) {
		LOGF("Compute output dump: staging allocation failed bytes=%" PRIu64 "\n", bytes);
		return;
	}

	vk::BufferImageCopy copy {};
	copy.bufferRowLength  = width;
	copy.bufferImageHeight = height;
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent      = {width, height, 1};
	image.Download({&copy, 1}, staging.buffer, 0, bytes);
	context.GetCommandScheduler().FinishCurrent();
	context.GetCommandScheduler().DrainPriorityOperations();

	void* mapped = nullptr;
	context.GetGraphics().MapMemory(staging.memory, mapped);
	if (mapped != nullptr) {
		double phase_ratio = 0.0;
		double phase_cv    = 0.0;
		if (image.info.bytes_per_block == 1u && width > 1u && height != 0u) {
			std::array<double, 32>   sums {};
			std::array<uint64_t, 32> counts {};
			const auto* pixels = static_cast<const uint8_t*>(mapped);
			for (uint32_t y = 0; y < height; y += 4u) {
				const auto* row = pixels + static_cast<uint64_t>(y) * width;
				for (uint32_t x = 1; x < width; x++) {
					const auto phase = x & 31u;
					sums[phase] += std::abs(static_cast<int32_t>(row[x]) -
					                        static_cast<int32_t>(row[x - 1u]));
					counts[phase]++;
				}
			}
			std::array<double, 32> means {};
			for (uint32_t i = 0; i < means.size(); i++) {
				means[i] = counts[i] != 0 ? sums[i] / static_cast<double>(counts[i]) : 0.0;
			}
			auto sorted = means;
			std::sort(sorted.begin(), sorted.end());
			const double median = (sorted[15] + sorted[16]) * 0.5;
			const double mean =
			    std::accumulate(means.begin(), means.end(), 0.0) / means.size();
			double variance = 0.0;
			for (const auto value: means) {
				variance += (value - mean) * (value - mean);
			}
			phase_ratio = median > 0.0 ? sorted.back() / median : 0.0;
			phase_cv = mean > 0.0 ? std::sqrt(variance / means.size()) / mean : 0.0;
		}
		const auto base = fmt::format("{}-{:016x}-r{}-{}x{}-bpb{}", file_prefix,
		                              program.shader_hash, resource, width, height,
		                              image.info.bytes_per_block);
		const auto output_dir = std::filesystem::path(directory);
		Common::File::CreateDirectories(output_dir);
		const auto raw_path = output_dir / (base + ".raw");
		Common::File raw;
		if (raw.Create(raw_path)) {
			raw.Write(mapped, static_cast<uint32_t>(bytes));
			raw.Close();
		}
		const auto info_path = output_dir / (base + ".txt");
		Common::File info;
		if (info.Create(info_path)) {
			info.Printf("shader_hash=%016" PRIx64 " resource=%u width=%u height=%u bpb=%u "
			            "guest_address=%010" PRIx64 " vk_format=%d guest_format=%u tile=%u "
			            "phase32_max_median=%.6f phase32_cv=%.6f\n",
			            program.shader_hash, resource, width, height,
			            image.info.bytes_per_block, image.info.data.address,
			            static_cast<int>(image.backing.format),
			            static_cast<uint32_t>(image.info.pixel_format), image.info.tile_mode,
			            phase_ratio, phase_cv);
			info.Close();
		}
		context.GetGraphics().UnmapMemory(staging.memory);
		LOGF("Compute output dump: wrote %s bytes=%" PRIu64
		     " phase32_max_median=%.3f phase32_cv=%.3f\n",
		     Common::PathToString(raw_path).c_str(), bytes, phase_ratio, phase_cv);
	}
	context.GetGraphics().DeleteBuffer(staging);
}

static bool ConsumeBinkTraceOccurrence(uint64_t shader_hash) {
	const char* const directory = std::getenv("KYTY_BINK_TRACE_DIR");
	if (directory == nullptr || directory[0] == '\0' ||
	    shader_hash != 0x0000000208a64d00ULL) {
		return false;
	}
	const char* const text = std::getenv("KYTY_BINK_TRACE_OCCURRENCE");
	const auto requested = text != nullptr ? std::max<uint64_t>(std::strtoull(text, nullptr, 10), 1u)
	                                       : 1u;
	static std::unordered_map<uint64_t, uint64_t> occurrences;
	return ++occurrences[shader_hash] == requested;
}

static void WriteBinkTrace(uint64_t shader_hash, vk::CommandBuffer command,
	                       RenderContext& context) {
	auto& gds = context.GetBufferCache().GetGdsBuffer();
	if (gds.Mapped().empty() || !gds.IsCoherent()) {
		LOGF("Bink trace: GDS buffer is not coherent host-mapped memory\n");
		return;
	}
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = gds.Handle();
	barrier.offset              = 0;
	barrier.size                = gds.Size();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eHost, {}, {}, barrier, {});
	context.GetCommandScheduler().FinishCurrent();
	context.GetCommandScheduler().DrainPriorityOperations();

	const char* const directory = std::getenv("KYTY_BINK_TRACE_DIR");
	const auto output_dir = std::filesystem::path(directory);
	Common::File::CreateDirectories(output_dir);
	const auto base = fmt::format("bink-trace-{:016x}", shader_hash);
	Common::File raw;
	const auto raw_path = output_dir / (base + ".bin");
	if (raw.Create(raw_path)) {
		raw.Write(gds.Mapped().data(), static_cast<uint32_t>(gds.Mapped().size()));
		raw.Close();
	}

	Common::File table;
	const auto table_path = output_dir / (base + ".tsv");
	uint32_t records = 0;
	if (table.Create(table_path)) {
		table.Printf("site\titeration\tlane\tpc\tvalue0\tvalue1\tvalue2\tvalue3\n");
		const auto* words = reinterpret_cast<const uint32_t*>(gds.Mapped().data());
		for (uint32_t site = 0; site < 7u; site++) {
			for (uint32_t iteration = 0; iteration < 4u; iteration++) {
				for (uint32_t lane = 0; lane < 64u; lane++) {
					const auto offset = ((site * 4u + iteration) * 64u + lane) * 8u;
					if (offset + 7u >= gds.Size() / sizeof(uint32_t) ||
					    words[offset] != (0xb17c0000u | site)) {
						continue;
					}
					table.Printf("%u\t%u\t%u\t%08" PRIx32 "\t%08" PRIx32 "\t%08" PRIx32
					             "\t%08" PRIx32 "\t%08" PRIx32 "\n",
					             site, iteration, lane, words[offset + 1u], words[offset + 4u],
					             words[offset + 5u], words[offset + 6u], words[offset + 7u]);
					records++;
				}
			}
		}
		table.Close();
	}
	LOGF("Bink trace: wrote %s records=%u\n", Common::PathToString(table_path).c_str(), records);
}

static bool ConsumeBinkReplayDumpOccurrence(uint64_t shader_hash) {
	const char* const directory = std::getenv("KYTY_BINK_REPLAY_DUMP_DIR");
	const char* const hash_text = std::getenv("KYTY_BINK_REPLAY_DUMP_HASH");
	const auto requested_hash = hash_text != nullptr && hash_text[0] != '\0'
	                                ? std::strtoull(hash_text, nullptr, 0)
	                                : 0x0000000208a64d00ULL;
	if (directory == nullptr || directory[0] == '\0' || shader_hash != requested_hash) {
		return false;
	}
	const char* const text = std::getenv("KYTY_BINK_REPLAY_DUMP_OCCURRENCE");
	const auto requested = text != nullptr ? std::max<uint64_t>(std::strtoull(text, nullptr, 10), 1u)
	                                       : 1u;
	static std::unordered_map<uint64_t, uint64_t> occurrences;
	return ++occurrences[shader_hash] == requested;
}

static uint32_t BinkReplayStorageResource(const ShaderRecompiler::IR::Program& program) {
	const char* const text = std::getenv("KYTY_BINK_REPLAY_STORAGE_RESOURCE");
	if (text != nullptr && text[0] != '\0') {
		return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		if (image.written &&
		    (image.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		     image.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint)) {
			return i;
		}
	}
	return UINT32_MAX;
}

static uint64_t BufferDescriptorSize(const ShaderBufferResource& descriptor);

static void WriteBinkReplayTransientBuffers(
    uint64_t shader_hash, const DescriptorCache::PreparedBindings& bindings,
    const std::vector<std::shared_ptr<Buffer>>& sources) {
	const char* const directory = std::getenv("KYTY_BINK_REPLAY_DUMP_DIR");
	if (directory == nullptr || directory[0] == '\0' || bindings.snapshot == nullptr) {
		return;
	}
	const auto count = std::min({bindings.resources.buffers.size(),
	                             bindings.snapshot->buffers.size(), sources.size()});
	const auto output_dir = std::filesystem::path(directory);
	Common::File::CreateDirectories(output_dir);
	for (uint32_t i = 0; i < count; i++) {
		if (sources[i] != nullptr) {
			continue;
		}
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(
		    bindings.snapshot->buffers[i]);
		const uint64_t size = BufferDescriptorSize(descriptor);
		if (descriptor.Base48() == 0 || size == 0 || size > UINT32_MAX) {
			continue;
		}
		std::vector<uint8_t> bytes(size);
		if (!Libs::LibKernel::Memory::TryReadGpuBacking(descriptor.Base48(), bytes.data(), size)) {
			LOGF("Bink replay dump: transient buffer[%u] has no readable guest backing\n", i);
			continue;
		}
		const auto path = output_dir / fmt::format("buffer-{}-{}-bytes.bin", i, size);
		Common::File file;
		if (file.Create(path)) {
			file.Write(bytes.data(), static_cast<uint32_t>(bytes.size()));
			file.Close();
			LOGF("Bink replay dump: wrote transient buffer[%u] bytes=%" PRIu64
			     " for shader=0x%016" PRIx64 "\n",
			     i, size, shader_hash);
		}
	}
}

static void WriteBinkReplayBuffers(uint64_t shader_hash, CommandBuffer& command,
	                               const DescriptorCache::PreparedBindings& bindings,
	                               const std::vector<std::shared_ptr<Buffer>>& sources,
	                               const std::vector<std::vector<uint8_t>>& guest_snapshots,
	                               const std::vector<uint8_t>& gpu_ownership_before_dispatch,
	                               RenderContext& context) {
	const char* const directory = std::getenv("KYTY_BINK_REPLAY_DUMP_DIR");
	if (directory == nullptr || directory[0] == '\0' || bindings.snapshot == nullptr) {
		return;
	}
	struct Copy {
		uint32_t index         = 0;
		uint64_t offset        = 0;
		uint64_t source_offset = 0;
		uint64_t size          = 0;
		Buffer*  source        = nullptr;
	};
	std::vector<Copy> copies;
	uint64_t total = 0;
	const auto count = std::min({bindings.resources.buffers.size(),
	                             bindings.snapshot->buffers.size(), sources.size()});
	for (uint32_t i = 0; i < count; i++) {
		const auto& view = bindings.resources.buffers[i];
		if (sources[i] == nullptr) {
			LOGF("Bink replay dump: buffer[%u] has no cache owner\n", i);
			continue;
		}
		auto* const source = sources[i].get();
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(
		    bindings.snapshot->buffers[i]);
		const auto address = descriptor.Base48();
		if (address == 0 || !source->IsInBounds(address, 1)) {
			continue;
		}
		const uint64_t source_offset = source->Offset(address);
		if (source_offset < view.offset) {
			continue;
		}
		const uint64_t alignment_adjustment = source_offset - view.offset;
		uint64_t size = static_cast<uint64_t>(descriptor.Stride()) * descriptor.NumRecords();
		if (view.range != VK_WHOLE_SIZE) {
			size = alignment_adjustment < view.range
			           ? std::min<uint64_t>(size, view.range - alignment_adjustment)
			           : 0;
		}
		size = std::min<uint64_t>(size, source->Size() - source_offset);
		if (size == 0) {
			continue;
		}
		copies.push_back({i, total, source_offset, size, source});
		total += size;
	}
	if (total == 0) {
		LOGF("Bink replay dump: no buffer bytes selected\n");
		return;
	}

	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	const auto [mapped, base_offset] = download.Map(total, 4u);
	if (mapped == nullptr) {
		LOGF("Bink replay dump: download reservation failed bytes=%" PRIu64 "\n", total);
		return;
	}
	for (const auto& copy: copies) {
		download.CopyFrom(command, *copy.source, copy.source_offset, base_offset + copy.offset,
		                  copy.size, vk::AccessFlagBits::eMemoryWrite, {},
		                  vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		                  vk::AccessFlagBits::eHostRead);
	}
	download.Commit();
	context.GetCommandScheduler().FinishCurrent();
	context.GetCommandScheduler().DrainPriorityOperations();
	download.Invalidate(base_offset, total);

	const auto output_dir = std::filesystem::path(directory);
	Common::File::CreateDirectories(output_dir);
	Common::File manifest;
	const auto manifest_path = output_dir / fmt::format("bink-replay-{:016x}.txt", shader_hash);
	if (manifest.Create(manifest_path)) {
		manifest.Printf("shader_hash=%016" PRIx64 " total_buffer_bytes=%" PRIu64 " buffers=%zu\n",
		                shader_hash, total, copies.size());
	}
	for (const auto& copy: copies) {
		const auto path = output_dir /
		                  fmt::format("buffer-{}-{}-bytes.bin", copy.index, copy.size);
		Common::File file;
		if (file.Create(path)) {
			file.Write(mapped + copy.offset, static_cast<uint32_t>(copy.size));
			file.Close();
		}
		if (!manifest.IsInvalid()) {
			const auto& view = bindings.resources.buffers[copy.index];
			manifest.Printf("buffer[%u] bytes=%" PRIu64 " native_offset=%" PRIu64
			                " native_range=%" PRIu64,
			                copy.index, copy.size, static_cast<uint64_t>(view.offset),
			                static_cast<uint64_t>(view.range));
		}

		// Compare the exact bytes visible to the Vulkan shader with the current guest/CPU
		// backing. Bink's coefficient buffers are CPU-produced and read-only in these
		// dispatches, so any mismatch here isolates cache upload/dirty-range handling before
		// shader translation or LDS arithmetic can affect the result.
		const bool guest_read = copy.index < guest_snapshots.size() &&
		                        guest_snapshots[copy.index].size() == copy.size;
		uint64_t differing_bytes = 0;
		if (guest_read) {
			const auto& guest_bytes = guest_snapshots[copy.index];
			for (uint64_t byte = 0; byte < copy.size; byte++) {
				differing_bytes += guest_bytes[byte] != mapped[copy.offset + byte] ? 1u : 0u;
			}
			const auto guest_path = output_dir /
			                        fmt::format("guest-buffer-{}-{}-bytes.bin", copy.index,
			                                    copy.size);
			Common::File guest_file;
			if (guest_file.Create(guest_path)) {
				guest_file.Write(guest_bytes.data(), static_cast<uint32_t>(guest_bytes.size()));
				guest_file.Close();
			}
		}
		if (!manifest.IsInvalid()) {
			const uint8_t gpu_ownership = copy.index < gpu_ownership_before_dispatch.size()
			                                  ? gpu_ownership_before_dispatch[copy.index]
			                                  : 0;
			manifest.Printf(" guest_read=%u differing_bytes=%" PRIu64
			                " gpu_buffer_dirty_before_dispatch=%u"
			                " gpu_image_dirty_before_dispatch=%u\n",
			                guest_read ? 1u : 0u, differing_bytes,
			                (gpu_ownership & 1u) != 0 ? 1u : 0u,
			                (gpu_ownership & 2u) != 0 ? 1u : 0u);
		}
	}
	if (!manifest.IsInvalid()) {
		manifest.Close();
	}
	LOGF("Bink replay dump: wrote %zu buffers bytes=%" PRIu64 " to %s\n", copies.size(),
	     total, Common::PathToString(output_dir).c_str());
}

static uint64_t BufferDescriptorSize(const ShaderBufferResource& descriptor) {
	const uint64_t records = descriptor.NumRecords();
	const uint64_t stride  = descriptor.Stride();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("compute buffer descriptor footprint overflow\n");
	}
	return stride == 0 ? records : records * stride;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const RenderCommandBuffer&    buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		if (!resource.written && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (program.info.buffers.size() != 1 || resources.buffers.size() != 1 ||
	    !program.info.images.empty() || !program.info.samplers.empty() ||
	    !program.info.addresses.empty() || !resources.images.empty() ||
	    !resources.samplers.empty() || !resources.addresses.empty()) {
		return false;
	}
	const auto& resource   = program.info.buffers.front();
	const auto& raw        = resources.buffers.front();
	const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(raw);
	if (!resource.formatted || !resource.written || resource.read || resource.atomic ||
	    resource.scalar || resource.max_byte_extent != 16 || descriptor.Stride() != 16 ||
	    descriptor.Format() != Prospero::BufferFormat::k32_32_32_32UInt ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    resource.packed_stride != descriptor.PackedStride() || raw.dword_count != 4 ||
	    program.user_data_base != 0 || resources.user_data.size() != 8) {
		return false;
	}
	for (uint32_t i = 0; i < raw.dword_count; i++) {
		if (raw.dwords[i] != resources.user_data[i]) {
			return false;
		}
	}
	const uint32_t clear = resources.user_data[4];
	if (resources.user_data[5] != clear || resources.user_data[6] != clear ||
	    resources.user_data[7] != clear) {
		return false;
	}
	const bool full_dispatch =
	    input.dispatch_thread_dimensions && input.threads_num[0] == 64 &&
	    input.threads_num[1] == 1 && input.threads_num[2] == 1 && group_x != 0 && group_y == 1 &&
	    group_z == 1 && input.dispatch_threads_num[0] == group_x &&
	    input.dispatch_threads_num[1] == 1 && input.dispatch_threads_num[2] == 1 &&
	    input.group_id[0] && !input.group_id[1] && !input.group_id[2] &&
	    // Prospero DispatchModifier bit 15 is clear in 0x61, so this is a
	    // wave64 dispatch. The recognized kernel has no wave-sensitive operation.
	    input.thread_ids_num == 1 && input.wave_size == 64 && !input.tg_size_en && mode == 0x61u &&
	    group_x % input.threads_num[0] == 0 && descriptor.NumRecords() == group_x;
	const auto size = BufferDescriptorSize(descriptor);
	if (!full_dispatch || size == 0) {
		return false;
	}
	resolved_descriptor = descriptor;
	resolved_clear      = clear;
	resolved_size       = size;
	return true;
}

static bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input, CommandBuffer& command,
                                        uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                        uint32_t mode) {
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeImageClear(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	auto& cache = command.GetContext().GetTextureCache();
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, RenderCommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (!ShaderAddressValid(sh_ctx.GetCs().cs_regs.data_addr)) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();
	const uint32_t sdk_guest_wave_size = Pm4::GetComputeWaveSizeFromDispatchModifier(mode);
	const uint32_t local_threads = std::max(cs_regs.cs_regs.num_thread_x, 1u) *
	                               std::max(cs_regs.cs_regs.num_thread_y, 1u) *
	                               std::max(cs_regs.cs_regs.num_thread_z, 1u);
	uint32_t guest_wave_size = sdk_guest_wave_size;
	const ShaderSubgroupCapabilities subgroup_caps {m_context.GetGraphics()};

	ShaderComputeInputInfo    input_info {};
	std::span<const uint32_t> cs_shader;
	// Compile native IR first so ordinary wave64/local64 kernels keep their
	// established scalar-pair and FlattenedMasks behavior. Recompile only exact
	// guest-wave programs into the logical single-workgroup representation.
	if (!ShaderCompileInfoCS(cs_regs, sh_regs, guest_wave_size,
	                         ShaderLaneMaskMode::NativeWave, input_info,
	                         cs_shader)) {
		EXIT("ShaderCompileInfoCS failed for dispatch with CS shader 0x%016" PRIx64 "\n",
		     cs_regs.cs_regs.data_addr);
	}
	auto lane_mask_mode = SelectComputeProgramLaneMaskMode(
	    subgroup_caps, guest_wave_size, local_threads, *input_info.stage.program);
	const uint32_t execution_wave_size = SelectComputeExecutionWaveSize(
	    subgroup_caps, local_threads, *input_info.stage.program);
	if (execution_wave_size != guest_wave_size) {
		guest_wave_size = execution_wave_size;
		if (!ShaderCompileInfoCS(cs_regs, sh_regs, guest_wave_size,
		                         ShaderLaneMaskMode::NativeWave, input_info, cs_shader)) {
			EXIT("ShaderCompileInfoCS wave32 compatibility fallback failed for CS shader 0x%016"
			     PRIx64 "\n",
			     cs_regs.cs_regs.data_addr);
		}
		lane_mask_mode = SelectComputeProgramLaneMaskMode(
		    subgroup_caps, guest_wave_size, local_threads, *input_info.stage.program);
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32u) {
			LOGF("Prospero wave64 CS 0x%016" PRIx64
			     " uses wave32 compatibility lowering for a %u-thread workgroup\n",
			     cs_regs.cs_regs.data_addr, local_threads);
		}
	}
	if (guest_wave_size == 64u) {
		static std::atomic<uint32_t> wave64_diagnostic_count {0};
		if (wave64_diagnostic_count.fetch_add(1, std::memory_order_relaxed) < 32u) {
			LOGF("Compute wave64 selection: cs=0x%016" PRIx64
			     " mode=0x%08x local=%ux%ux%u selected_masks=%u\n",
			     cs_regs.cs_regs.data_addr, mode, input_info.threads_num[0],
			     input_info.threads_num[1], input_info.threads_num[2],
			     static_cast<uint32_t>(lane_mask_mode));
			std::printf("Compute wave64 selection: cs=0x%016" PRIx64
			            " mode=0x%08x local=%ux%ux%u selected_masks=%u\n",
			            cs_regs.cs_regs.data_addr, mode, input_info.threads_num[0],
			            input_info.threads_num[1], input_info.threads_num[2],
			            static_cast<uint32_t>(lane_mask_mode));
		}
	}
	if (lane_mask_mode == ShaderLaneMaskMode::PerInvocation) {
		ShaderComputeInputInfo logical_info {};
		if (!ShaderCompileInfoCS(cs_regs, sh_regs, guest_wave_size, lane_mask_mode, logical_info,
		                         cs_shader)) {
			EXIT("ShaderCompileInfoCS logical wave64 recompile failed for CS shader 0x%016" PRIx64
			     "\n",
			     cs_regs.cs_regs.data_addr);
		}
		input_info = std::move(logical_info);
	}

	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	if (use_thread_dimensions) {
		input_info.dispatch_thread_dimensions = true;
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
	const bool     large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	DumpComputeResourceSnapshotOnce(input_info, sh_ctx.GetCs().cs_regs.data_addr, frame_num,
	                                thread_group_x, thread_group_y, thread_group_z, mode);
	if (TryConsumeComputeMetaClear(input_info, buffer)) {
		ResetBindings();
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		ResetBindings();
		return;
	}
	const auto sampled_images = std::count_if(
	    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(), program.bindings.push_constant_size);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     (image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
			      image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint)
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     std::max<uint32_t>(static_cast<uint32_t>(r.LastLevel()),
			                        static_cast<uint32_t>(r.MaxMip())) +
			         1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[i]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().CreateComputePipeline(input_info, sh_ctx.GetCs(), cs_shader);
	auto bindings = PrepareBindings(buffer, input_info.stage, vk::ShaderStageFlagBits::eCompute,
	                                DescriptorCache::Stage::Compute);
	RebindBuffers(buffer, bindings);
	RebindImages(buffer, bindings);
	TraceComputeImageAddressAccess(input_info, bindings, sh_ctx.GetCs().cs_regs.data_addr,
	                               submit_id, frame_num, thread_group_x, thread_group_y,
	                               thread_group_z, mode);
	const bool capture_bink_replay = ConsumeBinkReplayDumpOccurrence(program.shader_hash);
	const auto replay_storage_resource = BinkReplayStorageResource(program);
	std::vector<std::shared_ptr<Buffer>> replay_buffer_sources;
	std::vector<std::vector<uint8_t>>    replay_guest_buffers;
	std::vector<uint8_t>                 replay_gpu_ownership;
	if (capture_bink_replay) {
		replay_buffer_sources.reserve(bindings.resources.buffers.size());
		replay_guest_buffers.resize(bindings.resources.buffers.size());
		replay_gpu_ownership.resize(bindings.resources.buffers.size());
		for (uint32_t i = 0; i < bindings.resources.buffers.size(); i++) {
			const auto& view = bindings.resources.buffers[i];
			replay_buffer_sources.push_back(view.owner != nullptr
			                                    ? std::static_pointer_cast<Buffer>(view.owner)
			                                    : nullptr);
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(
			    bindings.snapshot->buffers[i]);
			uint64_t size = BufferDescriptorSize(descriptor);
			if (view.range != VK_WHOLE_SIZE) {
				size = std::min<uint64_t>(size, view.range);
			}
			const auto& source = replay_buffer_sources.back();
			if (source != nullptr) {
				const auto address = descriptor.Base48();
				if (address == 0 || !source->IsInBounds(address, 1)) {
					size = 0;
				} else {
					const uint64_t source_offset = source->Offset(address);
					if (source_offset < view.offset) {
						size = 0;
					} else {
						const uint64_t alignment_adjustment = source_offset - view.offset;
						if (view.range != VK_WHOLE_SIZE) {
							size = alignment_adjustment < view.range
							           ? std::min<uint64_t>(
							                 size, view.range - alignment_adjustment)
							           : 0;
						}
						size = std::min<uint64_t>(size, source->Size() - source_offset);
					}
				}
			}
			if (descriptor.Base48() != 0 && size != 0 && size <= UINT32_MAX) {
				auto& buffer_cache = m_context.GetBufferCache();
				auto& texture_cache = m_context.GetTextureCache();
				const bool gpu_buffer_dirty =
				    buffer_cache.HasGpuDirtyBytes(descriptor.Base48(), size);
				const bool gpu_image_dirty =
				    texture_cache.QueryRegion(descriptor.Base48(), size).gpu_image_bytes;
				replay_gpu_ownership[i] = static_cast<uint8_t>((gpu_buffer_dirty ? 1u : 0u) |
				                                                (gpu_image_dirty ? 2u : 0u));
				auto& bytes = replay_guest_buffers[i];
				bytes.resize(size);
				if (!Libs::LibKernel::Memory::TryReadGpuBacking(descriptor.Base48(), bytes.data(),
				                                                  size)) {
					bytes.clear();
				}
			}
		}
		WriteBinkReplayTransientBuffers(program.shader_hash, bindings, replay_buffer_sources);
		const char* const replay_directory = std::getenv("KYTY_BINK_REPLAY_DUMP_DIR");
		if (replay_directory != nullptr && replay_directory[0] != '\0') {
			const auto pre_storage_directory =
			    std::filesystem::path(replay_directory) / "pre-storage-image";
			const auto pre_storage_text = Common::PathToString(pre_storage_directory);
			// The Bink kernel may leave unchanged macroblocks untouched in its ping-pong
			// destination. Preserve the destination before this one-shot diagnostic
			// dispatch so the standalone replay starts from the same image contents.
			WriteComputeOutputDump(program, bindings, m_context, pre_storage_text.c_str(),
			                       replay_storage_resource,
			                       "compute-pre-storage");
		}
	}

	auto vk_buffer = buffer.Handle();
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline.pipeline_layout, bindings);
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       (image.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		                        image.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint);
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	if (capture_bink_replay) {
		WriteBinkReplayBuffers(program.shader_hash, buffer, bindings, replay_buffer_sources,
		                       replay_guest_buffers, replay_gpu_ownership, m_context);
		const char* const replay_directory = std::getenv("KYTY_BINK_REPLAY_DUMP_DIR");
		if (replay_directory != nullptr && replay_directory[0] != '\0') {
			const auto post_storage_directory =
			    std::filesystem::path(replay_directory) / "post-storage-image";
			const auto post_storage_text = Common::PathToString(post_storage_directory);
			WriteComputeOutputDump(program, bindings, m_context, post_storage_text.c_str(),
			                       replay_storage_resource,
			                       "compute-post-storage");
		}
	}
	if (ConsumeBinkTraceOccurrence(program.shader_hash)) {
		WriteBinkTrace(program.shader_hash, vk_buffer, m_context);
	}
	if (ConsumeComputeOutputDumpOccurrence(program.shader_hash)) {
		WriteComputeOutputDump(program, bindings, m_context);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
