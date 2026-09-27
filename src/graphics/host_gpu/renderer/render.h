#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/upscaler/upscaler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart.
struct DrawIndirectArgs {
	uint64_t              address = 0;
	uint32_t              count   = 0;
	uint32_t              stride  = 0;
	ShaderIndirectOffsets offsets;
};

struct DrawIndexArgs {
	uint32_t         index_count         = 0;
	const void*      index_addr          = nullptr;
	uint32_t         instance_count      = 0;
	uint32_t         index_type_and_size = 0;
	int32_t          base_vertex         = 0;
	uint32_t         first_instance      = 0;
	DrawOffsetSource offset_source       = DrawOffsetSource::DrawState;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
};

struct DrawAutoArgs {
	uint32_t         vertex_count   = 0;
	uint32_t         instance_count = 0;
	uint32_t         first_vertex   = 0;
	uint32_t         first_instance = 0;
	DrawOffsetSource offset_source  = DrawOffsetSource::DrawState;
	DrawIndirectArgs indirect;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept { return m_buffer == nullptr; }

	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;

	[[nodiscard]] vk::CommandBuffer Handle() const noexcept { return m_buffer; }
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer = nullptr;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
};

namespace Pssr {
struct Request;
}

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y,
	                    uint32_t thread_group_z, uint32_t mode, bool async_compute);
	void DispatchIndirect(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
	                      bool async_compute);
	void ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);
	// table_candidate (bindless table elements) binds a null image for a texture whose guest
	// memory cannot be read, as tables keep the T#s of freed textures, and for one whose image the
	// texture cache declines to recreate or cannot view as the T# reads it; a depth T# over a
	// color image binds a depth copy of it. examined receives the guest range before the lookup
	// depends on the texture cache: a null image without it depends only on the T# and the guest
	// backing.
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value,
	                                            bool        table_candidate = false,
	                                            GuestRange* examined        = nullptr);

private:
	// False, with nothing drawn, when the draw's indirect records need CPU counts.
	bool DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args);
	bool DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		PreparedBindings                pixel;
	};

	// Bindless table elements resolved for one root image, by T#. Most of a table is unchanged
	// from dispatch to dispatch, and resolving an element repeats the same texture-cache lookup
	// while no image in its range was registered or unregistered and the guest backing did not
	// change. Elements with DCC metadata are resolved again on every use: their lookup
	// materializes fast clears.
	struct TableElementResolution {
		// What every preparation of a table reads comes first, on one cache line: a table names
		// thousands of elements.
		// The PrepareBindings() call that last bound the element, and its index there.
		uint64_t       prepared_stamp = 0;
		uint32_t       prepared_index = 0;
		// The element's counter in m_table_element_versions (kept while the element lives).
		uint32_t       version_slot  = UINT32_MAX;
		bool           permanent     = false; // Null for the root's view, whatever the memory.
		bool           volatile_dcc  = false;
		// A resolved element depends on the guest backing only through whether its data and
		// metadata are readable: when the backing epoch moves (any map or unmap anywhere,
		// constantly while a game streams), the element stays valid while that answer holds.
		bool           backing_checkable = false;
		bool           backing_readable  = false;
		uint64_t       image_epoch   = 0;
		uint64_t       backing_epoch = 0;
		// The global image-set epoch at which the element was last known valid: while it holds,
		// no image anywhere changed and the range needs no check.
		uint64_t       checked_epoch = 0;
		GuestRange     range; // Guest memory whose images decide the lookup, if any.
		GuestRange     metadata_range;
		TableViewMemo  view;
		TextureBinding texture;
	};
	struct TableDescriptorHash {
		size_t operator()(const std::array<uint32_t, 8>& dwords) const noexcept;
	};
	using TableElements =
	    std::unordered_map<std::array<uint32_t, 8>, TableElementResolution, TableDescriptorHash>;
	// The element the last preparation of a root found for one slot: a slot whose T# did not
	// change skips the lookup by T#.
	struct TableSlot {
		std::array<uint32_t, 8> dwords {};
		TableElementResolution* element = nullptr;
		// The element's version_slot, read without reading the element.
		uint32_t                version_slot = UINT32_MAX;
	};
	struct TableResolution {
		ShaderRecompiler::IR::ImageResource root;
		uint64_t                            last_use = 0;
		TableElements                       elements;
		std::vector<TableSlot>              slots;
	};

	// `stamp` identifies the PrepareBindings() call: an element it already resolved is not
	// resolved again for another slot. `known` is the element of `value`, when the caller has it.
	[[nodiscard]] TableElementResolution&
	ResolveTableElement(TableResolution& table, const ShaderRecompiler::IR::DescriptorValue& value,
	                    uint64_t stamp, TableElementResolution* known = nullptr);
	[[nodiscard]] TableResolution& FindTableResolution(const ShaderRecompiler::IR::ImageResource& root);
	[[nodiscard]] bool TableElementBackingReadable(const TableElementResolution& element);
	// Advances the element's counter: its texture, view or layout changed (see
	// TableBindingState::Checked).
	void TableElementChanged(const TableElementResolution& element) {
		if (element.version_slot < m_table_element_versions.size()) {
			m_table_element_versions[element.version_slot]++;
		}
	}

	// The bindless tables a program bound last. GTA V's ray tracing dispatches bind ~16k slots
	// naming ~8k textures each, and almost all of them stay the same from frame to frame: while
	// the T#s of every slot are unchanged, a binding of the program revisits only the elements
	// that an image registration or a guest backing change may have affected, and the elements
	// whose image is no longer bound as before (see ReuseTableBindings()). The images of the
	// elements stay pinned in the texture cache meanwhile: they are not touched every binding.
	struct TableBindingState {
		struct Range {
			ShaderRecompiler::IR::ImageResource root;
			TableResolution*                    resolution = nullptr;
			uint32_t                            table      = 0;
			uint32_t                            capacity   = 0;
			// The ImageTableSnapshot::slots_id the range's values were taken from (0: unknown).
			uint64_t                            slots_id   = 0;
		};
		struct Element {
			TableElementResolution*               resolution = nullptr;
			TableResolution*                      table      = nullptr;
			ShaderRecompiler::IR::DescriptorValue source;
			uint32_t                              root = 0;
			// Image the element is pinned to.
			ImageId                               pinned;
			// Visited by every binding (DCC metadata), or whenever the guest backing changes
			// (the element's resolution does not record whether its backing is readable).
			bool always            = false;
			bool unchecked_backing = false;
			// The ranges `spans` holds for the element.
			GuestRange span_range;
			GuestRange span_metadata;
		};
		// The guest pages (in texture-cache page granularity) of an element's data or metadata.
		struct PageSpan {
			uint64_t first   = 0;
			uint64_t last    = 0;
			uint32_t element = 0;
		};
		std::vector<Range>                   ranges;
		// The T# of every slot of every range, in binding order.
		std::vector<ShaderRecompiler::IR::DescriptorValue> values;
		std::vector<Element>                 elements;
		// The element of every slot.
		std::vector<uint32_t>                slots;
		// The descriptor of every element, and of every slot.
		std::vector<vk::DescriptorImageInfo> infos;
		std::vector<vk::DescriptorImageInfo> slot_infos;
		// The slots of each element: element_slots[element_slot_begin[e] .. [e + 1]).
		std::vector<uint32_t>                element_slot_begin;
		std::vector<uint32_t>                element_slots;
		// The element of each element resolution, by its version_slot (UINT32_MAX: none). The
		// elements of a state that applies are alive, so their version slots are theirs alone
		// (see ResetBindings()).
		std::vector<uint32_t>                element_of_version;
		// Element spans by first page; `max_span` is the longest (last - first).
		std::vector<PageSpan>                spans;
		uint64_t                             max_span = 0;
		// Elements visited by every binding, and whenever the guest backing changes.
		std::vector<uint32_t>                always;
		std::vector<uint32_t>                unchecked_backing;
		// Elements marked for a visit by this binding.
		std::vector<uint64_t>                marks;
		uint64_t                             mark_stamp = 0;
		// What RebindReusedTables() last found an element unchanged with: the version of its
		// image (TextureCache::ImageVersion()) and of its resolution (TableElementChanged()).
		// While both hold and the operation does not bind the image itself, the element is still
		// unchanged: the check reads neither the resolution nor the image (GTA V's ray tracing
		// tables have ~16 000 elements, scattered in memory, and almost none change).
		struct Checked {
			static constexpr uint32_t None      = UINT32_MAX; // Check the element.
			static constexpr uint32_t NullImage = UINT32_MAX - 1;
			uint32_t                  image           = None; // ImageId index, or NullImage.
			uint32_t                  image_version   = 0;
			uint32_t                  element         = 0; // TableElementResolution::version_slot.
			uint32_t                  element_version = 0;
		};
		std::vector<Checked>                 checked;
		uint64_t                             generation = 0;
		uint64_t                             set_epoch  = 0;
		uint64_t                             backing_epoch = 0;
		bool                                 valid = false;
		// The descriptors of the table slots (`slot_infos`) in a set that is never bound
		// (DescriptorHeap::CommitPersistent()): a binding that reuses the state copies its table
		// ranges from it and writes only the slots whose descriptors changed into it.
		vk::DescriptorSet                    slot_set;
		bool                                 slot_set_current = false;
	};
	// Prepares the tables of `prepared` from the program's state when it still applies; false
	// when the tables must be resolved slot by slot.
	[[nodiscard]] bool ReuseTableBindings(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                                      const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
	                                      TableBindingState& state, uint64_t stamp,
	                                      PreparedBindings& prepared);
	// Marks the elements a change of the image set or of the guest backing since the state's
	// epochs may affect, and moves the epochs to now.
	void MarkTableChanges(TableBindingState& state);
	void MarkTableElement(TableBindingState& state, uint32_t element);
	void MarkTablePages(TableBindingState& state, uint64_t address, uint64_t size);
	void RebuildTableSpans(TableBindingState& state);
	// Adds the spans of an element that joined the state after the others (unsorted).
	static void AddTableSpans(TableBindingState& state, uint32_t element);
	// Sorts the spans AddTableSpans() added after the first `sorted` into place.
	void MergeTableSpans(TableBindingState& state, size_t sorted);
	// The slots of each element (element_slot_begin / element_slots) from `slots`.
	static void IndexTableSlots(TableBindingState& state);
	// ReuseTableBindings() of tables whose T#s changed in some slots: binds those slots to the
	// elements of their new T#s. New elements join the state and are marked; a slot that names
	// an element the state has goes to m_table_moved_slots.
	void PatchTableSlots(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                     const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
	                     TableBindingState& state, uint64_t stamp);
	// Drops the elements no slot names any more (and unpins their images); the binding's marks
	// follow the elements it keeps.
	void DropUnusedTableElements(TableBindingState& state);
	// After CommitBindings() bound the tables: records the descriptors and pins the images.
	void FinishTableBindings(TableBindingState& state, const PreparedBindings& prepared);
	void UnpinTableBindings(TableBindingState& state);
	// RebindImages() of reused tables: visits the elements PrepareBindings() marked and those
	// whose image is no longer bound as the state records.
	void RebindReusedTables(PreparedBindings& prepared);

	// ResolveTexture's image descriptions by T# and shader view (see DeriveTextureDesc).
	struct TextureDescKey {
		std::array<uint32_t, 8> dwords {};
		std::array<uint32_t, 5> view {};
		bool                    operator==(const TextureDescKey&) const = default;
	};
	struct TextureDescKeyHash {
		size_t operator()(const TextureDescKey& key) const noexcept;
	};
	[[nodiscard]] static TextureDescKey
	MakeTextureDescKey(const ShaderRecompiler::IR::ImageResource&   resource,
	                   const ShaderRecompiler::IR::DescriptorValue& value);
	// Writes the image and view the last lookup of a description found into `binding`, when
	// they still hold (see TextureDescDerivation::Found).
	[[nodiscard]] bool ReuseTextureLookup(TextureDescDerivation& derived, TextureBinding& binding);
	static constexpr size_t MaxTextureDescs = size_t {1} << 16u;
	std::unordered_map<TextureDescKey, TextureDescDerivation, TextureDescKeyHash> m_texture_descs;
	// Bindings point into m_texture_descs until their views are acquired: a full map is cleared
	// once the bindings of the operation are reset.
	bool m_clear_texture_descs = false;
	// Advances when m_texture_descs is cleared (bindings keep pointers into it).
	uint64_t m_texture_descs_generation = 1;

	// The host sampler for a sampler binding; see NativeSampler().
	[[nodiscard]] vk::Sampler NativeSampler(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                                        uint32_t                                        index,
	                                        const ShaderRecompiler::IR::DescriptorValue&    value);
	struct SamplerMemoEntry {
		std::array<uint32_t, 4> fields {};
		bool                    integer_border = false;
		vk::Sampler             sampler        = nullptr;
	};
	std::array<SamplerMemoEntry, 256> m_sampler_memo {};

	[[nodiscard]] TextureBinding
	ResolveTableTexture(const ShaderRecompiler::IR::ImageResource&   root,
	                    const ShaderRecompiler::IR::DescriptorValue& value,
	                    GuestRange*                                  examined = nullptr);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slot, bool ignore_target_mask = false,
	                              bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                    const DrawEmitInfo& emit, const DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                          DrawRenderState& state);
	void               ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                       const DrawRenderState& state, const DrawEmitInfo& emit,
	                                       const DrawIndexBufferSource& index_source);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                        CommandBuffer& command, uint32_t group_x,
	                                        uint32_t group_y, uint32_t group_z, uint32_t mode);
	[[nodiscard]] bool        Upscale(vk::CommandBuffer command, const Pssr::Request& request,
	                                  Dlss::Inputs& inputs);

	// A null feature is one the upscaler refused or failed.
	struct UpscalerContext {
		uint64_t                           context;
		Dlss::Creation                     creation;
		std::unique_ptr<Upscaler::Feature> feature;
	};

	// GET_LOD_STATS counters since their last reset. With --lod-stats a bound T# with counting
	// enabled counts as sampled at its BASE_LEVEL, clamped when its MIN_LOD lies above that level.
	struct MipStatsCounter {
		uint8_t finest_level = 15; // no samples
		bool    clamped      = false;
		bool    unclamped    = false;
	};

	RenderContext&                        m_context;
	bool                                  m_lod_stats = Config::LodStatsEnabled();
	GraphicsBindings                     m_graphics_bindings;
	// The current draw's targets; PrepareDrawRenderState resolves them again for each draw.
	DrawTargets                           m_targets;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	// Table ranges copied from the slot sets of the table states (see CommitBindings()), and the
	// slots written into those sets first.
	std::vector<vk::CopyDescriptorSet>    m_descriptor_copies;
	std::vector<vk::WriteDescriptorSet>   m_slot_set_writes;
	vk::DescriptorSetLayout               m_pssr_descriptor_layout = nullptr;
	vk::PipelineLayout                    m_pssr_pipeline_layout   = nullptr;
	vk::Pipeline                          m_pssr_pipeline          = nullptr;
	std::vector<UpscalerContext>          m_upscaler_contexts;
	std::array<MipStatsCounter, 256>      m_mip_stats {};
	// Prepared bindings and slot memos point into the elements: a deque never relocates its
	// entries (std::unordered_map has no noexcept move, so a growing vector copied the elements
	// and left those pointers dangling).
	std::deque<TableResolution>           m_table_resolutions;
	// Elements a table resolution dropped while prepared bindings may still point into them;
	// freed by ResetBindings().
	std::deque<TableElements>             m_retired_table_elements;
	uint64_t                              m_table_resolution_tick = 0;
	uint64_t                              m_table_prepare_stamp   = 0;
	std::vector<vk::DescriptorImageInfo>  m_table_image_infos;
	// Advances whenever a table resolution drops its elements: table binding states pointing
	// into them no longer apply.
	uint64_t                              m_table_generation = 0;
	std::unordered_map<const void*, TableBindingState> m_table_states;
	std::vector<std::pair<uint64_t, uint64_t>>         m_table_changes;
	std::vector<uint32_t>                              m_table_marked;
	// MergeTableSpans(): the merged spans (swapped with the state's).
	std::vector<TableBindingState::PageSpan>           m_table_spans;
	// DropUnusedTableElements(): the new index of every element (UINT32_MAX: dropped).
	std::vector<uint32_t>                              m_table_remap;
	// ReuseTableBindings(): the slots whose T# changed (slot, range, slot in its range).
	struct ChangedTableSlot {
		uint32_t slot  = 0;
		uint32_t range = 0;
		uint32_t index = 0;
	};
	std::vector<ChangedTableSlot>                      m_table_changed_slots;
	// PatchTableSlots(): the changed slots that name an element the state already had.
	std::vector<uint32_t>                              m_table_moved_slots;
	// RebindReusedTables(): the elements whose versions moved, and whether their binding changed
	// (one byte per element checked).
	std::vector<uint32_t>                              m_table_checks;
	std::vector<uint8_t>                               m_table_flags;
	// A counter per table element resolution (TableElementResolution::version_slot); freed
	// counters are reused.
	std::vector<uint32_t>                              m_table_element_versions;
	std::vector<uint32_t>                              m_free_table_element_versions;
	// RebindReusedTables(): the images the operation binds so far (m_bound_images), by ImageId
	// index: their binding state is the operation's.
	std::vector<uint64_t>                              m_bound_image_marks;
	uint64_t                                           m_bound_image_stamp = 0;
	// Reused by every draw; see AcquireDrawRenderState().
	std::unique_ptr<DrawRenderStorage, void (*)(DrawRenderStorage*)> m_draw_state {nullptr,
	                                                                               nullptr};
	// Resolutions ahead of execution; see ResolveDrawAhead().
	std::unique_ptr<ResolvedDrawRing, void (*)(ResolvedDrawRing*)> m_resolved_ring {nullptr,
	                                                                                nullptr};
	ResolvedDraw*                                                  m_resolved_draw = nullptr;
	uint64_t                                                       m_context_id    = 0;
	// The context snapshot whose registers hw_check() last accepted.
	uint64_t m_checked_context_id = 0;
	// The last color-target descriptions per slot; see ResolveRenderColorTarget().
	std::unique_ptr<ColorTargetMemo, void (*)(ColorTargetMemo*)> m_color_target_memo {nullptr,
	                                                                                  nullptr};
	// The last depth-target descriptions; see ResolveRenderDepthTarget().
	std::unique_ptr<DepthTargetMemo, void (*)(DepthTargetMemo*)> m_depth_target_memo {nullptr,
	                                                                                  nullptr};
	// The render targets of the last draw; see RenderTargetMemo.
	std::unique_ptr<RenderTargetMemo, void (*)(RenderTargetMemo*)> m_render_target_memo {nullptr,
	                                                                                     nullptr};
	// Whether the executing draw reused the render targets of the draw before.
	bool     m_render_targets_reused = false;
	uint64_t m_draw_window           = 0;
	std::unique_ptr<IndirectThreadPass, void (*)(IndirectThreadPass*)> m_indirect_thread_pass {
	    nullptr, nullptr};

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
