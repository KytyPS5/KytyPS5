// --- Table draws (GPU-driven XPR, docs/GPU-DRIVEN-XPR.md) ------------------------------
//
// chenxiao07's src/local/table-xpr.inc at 952cbd9ca (13-11-2026), ported to this tree (cxCompat.h). Not ported yet:
// the scout (KYTY_SCOUT), table dispatches (KYTY_CX_DISPATCH), address probe indirect images (KYTY_CX_INDIRECT) and the
// speculation (KYTY_SPECULATE=4), which is not on by default in his tree either.
#ifndef KYTY_CX_DISPATCH
#define KYTY_CX_DISPATCH 1
#endif
#ifndef KYTY_CX_INTERCHANGE
#define KYTY_CX_INTERCHANGE 0
#endif
#ifndef KYTY_CX_INDIRECT
#define KYTY_CX_INDIRECT 0
#endif
#ifndef KYTY_CX_SCOUT
#define KYTY_CX_SCOUT 1
#endif
using HashedKey = NativeXprKeyHash;

// An image's validity: what moves when a binding of one of its views depends on something else (this tree's per-slot
// version counter, which StateChanged advances: contents, tracking, identity, layout).
static uint64_t TableValidity(const Image& image) {
	return image.version != nullptr ? image.version->load(std::memory_order_acquire) : 0;
}

// The GS user SGPR the CP writes an indirect draw's start instance into (START_INST_LOC): his vertex programs are
// translated for it. Not carried by this tree's registers yet: -1, none.
// (This tree's program-cache and specialization helpers of the table path, from the first port.)
namespace {
std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
TableDrawExportMapping(const GraphicContext& graphics, const HW::Context& ctx, uint32_t color_output_mask) {
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX> mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			mapping[slot] =
			    TextureGetRenderTargetFormat(graphics, rt.info.format, rt.info.channel_type, rt.info.channel_order)
			        .export_mapping;
		}
	}
	return mapping;
}

bool TableStridesEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_TABLE_STRIDES");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// (`runtime_format`: the program decodes the buffer's format from its buffer word when it is specialized for none.)
bool TableBufferSpecialized(const ShaderRecompiler::IR::BufferResource&                 resource,
                            const ShaderRecompiler::IR::ResourceSpecialization::Buffer& specialized,
                            const std::array<uint32_t, 4>& words4, bool runtime_format) {
	runtime_format = runtime_format && specialized.descriptor_format == Prospero::BufferFormat::kInvalid;
	ShaderBufferResource descriptor;
	std::copy_n(words4.begin(), 4, descriptor.fields);
	if (descriptor.Type() != 0) descriptor = {};
	auto       packed_stride = descriptor.PackedStride();
	const auto stride        = packed_stride & 0x3fffu;
	const bool swizzle       = stride != 0u && ((packed_stride >> 14u) & 1u) != 0u;
	if (stride == 0u) {
		packed_stride &= ~((1u << 14u) | (3u << 16u));
	} else if (!swizzle) {
		packed_stride &= ~(3u << 16u);
	}
	// (KYTY_TABLE_STRIDES: an unswizzled buffer of another stride, which table programs read from the block.)
	const bool any_stride = TableStridesEnabled() && !swizzle && (specialized.packed_stride & (1u << 14u)) == 0u &&
	                        resource.indirect_root == ShaderRecompiler::IR::BufferResource::NoIndirectBuffer &&
	                        resource.indirect_resources.empty();
	const auto stride_bits = any_stride ? ~0x3fffu : ~0u;
	return (packed_stride & stride_bits) == (specialized.packed_stride & stride_bits) &&
	       (runtime_format ||
	        ((resource.formatted ? descriptor.Format() : Prospero::BufferFormat::kInvalid) == specialized.descriptor_format &&
	         (resource.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7)) == specialized.descriptor_swizzle)) &&
	       (descriptor.OutOfBounds() == 0u && stride == 0u) == specialized.zero_stride_oob &&
	       (resource.scalar && descriptor.GetSize() == 0u) == specialized.scalar_empty;
}

const ShaderRecompiler::IR::ResourceSnapshot g_table_no_resources {};

} // namespace

// A run of dwords of guest memory as the draw would read it now; false where the normal path must read it (nothing
// mapped there, or bytes the GPU wrote that only a readback gives).
static bool TableReadRun(uint64_t address, uint32_t count, uint32_t* out) {
	const uint64_t bytes = uint64_t {count} * 4u;
	if ((address & 3u) != 0 || !GuestRange {address, bytes}.Valid()) return false;
	if (Libs::LibKernel::Memory::MayFaultOnGpuRead(address, bytes))
		return Libs::LibKernel::Memory::TryReadAroundGpuWrites(address, out, bytes);
	return Libs::LibKernel::Memory::TryReadBacking(address, out, bytes);
}

static thread_local int32_t g_table_offsets_key = -1;
static int32_t TableStartInstance(const HW::VertexShaderInfo&) {
	return g_table_offsets_key;
}
static bool TableMemoOn() {
	return kyty_local_table_memo.load(std::memory_order_relaxed) != 0;
}
static uint32_t TableMemoVerifyEvery() {
	return kyty_local_table_memo_verify.load(std::memory_order_relaxed);
}
static uint32_t TableSerialsMode() {
	return kyty_local_table_serials.load(std::memory_order_relaxed);
}
//
// Included by renderDraw.cpp after native-xpr.inc (local Vulkan recording builds only).
//
// KYTY_TABLE_XPR=1: the XPR draws of a shader pair whose programs allow table mode
// (ShaderRecompiler::IR::EnterTableMode) need no record. For each draw the renderer evaluates the programs' table
// plans (IR::TablePlan): the slots their shaders read and the T#/S# words, from guest memory as it is when the draw
// is translated (as the normal path's SRT evaluation, which the game's command buffers patch between draws); the
// T#/S# words pick the pair's image set - a descriptor set with the images and the samplers, proven current once
// a frame - and each V# is resolved to its buffer's device address (cached while its buffer and region stay the
// same). The slots and buffer addresses go into a block in the table upload ring whose device address is pushed
// with the user data: the shaders read it, no flattened SRT binding, no buffer descriptors.
//
// The normal path, asked to store (TableStore), compiles the pair's table mode programs and the pipeline of each
// draw state, and captures the draw state (NativeXprCaptureState).
//
// KYTY_TABLE_XPR=2 also continues a run (TableContinue): a draw the observer calls clean (no register but user
// data and index state changed) right after a table draw whose emission is the last one in the command buffer
// has the same pair and draw state, with its pipeline and dynamic state still bound.

// Roles 1.. (the speculation threads': RenderExecutor::Role) of a pair's or a set's per-role values, in a block of its
// own allocated at a speculation thread's first use (KYTY_SPECULATE=4); role 0 (the GPU thread) keeps its value inline,
// so in mode 0 the pair and the set are the base's fields and size, and nothing is allocated. Moved with its owner.
template <typename T>
struct TableRoleSide {
	std::atomic<T*> block {nullptr};
	TableRoleSide() = default;
	TableRoleSide(TableRoleSide&& other) noexcept: block(other.block.exchange(nullptr)) {}
	TableRoleSide& operator=(TableRoleSide&& other) noexcept {
		delete block.exchange(other.block.exchange(nullptr));
		return *this;
	}
	TableRoleSide(const TableRoleSide&)            = delete;
	TableRoleSide& operator=(const TableRoleSide&) = delete;
	~TableRoleSide() { delete block.load(); }
	[[nodiscard]] T* Peek() const noexcept { return block.load(std::memory_order_acquire); }
	T& Get() {
		return Get([](T&) {});
	}
	// (`init` fills a block before it is published.)
	template <typename Init>
	T& Get(Init&& init) {
		T* at = block.load(std::memory_order_acquire);
		if (at != nullptr) return *at;
		auto* made = new T;
		init(*made);
		if (block.compare_exchange_strong(at, made, std::memory_order_acq_rel)) return *made;
		delete made;
		return *at;
	}
};

// A draw state's pipeline.
struct RenderExecutor::TableVariant {
	uint64_t                       state          = 0;
	const PipelineCache::Pipeline* pipeline       = nullptr; // null while a worker compiles it
	bool                           fetch_embedded = false;   // the vertex program fetches (a direct draw's offsets)
	bool                           refused        = false;   // another specialization than the pair's programs
	uint32_t                       colors         = 0;       // the color attachments its pipeline was made for
	bool                           has_depth      = false;   // the depth and stencil attachments it was made for
	bool                           has_stencil    = false;
	// The pixel program's export mapping under this state (GetGraphicsPrograms).
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX> mapping {};
};

// Words a draw writes whole after resizing (TableEvaluate's T#/S# words): resize leaves the new ones uninitialized. (With
// std::allocator it zeroed them first, a memset call per draw: TableEvaluate 7.1 -> 6.8% of the GPU thread at Latria.)
template <typename T>
struct TableUninitialized: std::allocator<T> {
	using std::allocator<T>::allocator;
	template <typename U>
	struct rebind {
		using other = TableUninitialized<U>;
	};
	template <typename U>
	void construct(U* at) noexcept {
		::new (static_cast<void*>(at)) U;
	}
	template <typename U, typename... Args>
	void construct(U* at, Args&&... args) {
		::new (static_cast<void*>(at)) U(std::forward<Args>(args)...);
	}
};
using TableWords = std::vector<uint32_t, TableUninitialized<uint32_t>>;

// The descriptor set of one choice of images and samplers (the T#/S# words of every stage).
struct RenderExecutor::TableImageSet {
	struct Image {
		ImageId                 id;
		uint64_t                serial = 0;
		vk::ImageLayout         layout = vk::ImageLayout::eUndefined;
		ImageSubresourceRange   range;
		TextureCache::ImageDesc desc;
		vk::AccessFlags2        access = vk::AccessFlagBits2::eShaderRead;
		bool                    target  = false; // a target of the draw state the set was made under (its access)
		// A target the shaders sample (KYTY_TABLE_TARGET_READ): the shader access added to the target's (in `access`).
		vk::AccessFlags2        target_read {};
		bool                    storage = false; // the shader writes it: GPU-modified at every use (TableTransit)
		bool                    whole   = false; // bound both as a texture and as storage: the whole image's state
		// The main executor's proof of its use (a target, or a texture without DCC found current): the image's validity
		// then, which the proof holds with while no registration stamped the view's pages since (TableValidateSet).
		bool                    proven   = false;
		uint64_t                validity = 0;
		// TextureCache::ColorClearSettled's memo for a DCC image's fast clears (TableTransit).
		mutable uint64_t        clear_memo = 0;
	};
	TableWords            words; // without the LOD statistics fields (NativeXprLodStatsMask)
	vk::DescriptorSet     set  = nullptr;
	vk::DescriptorPool    pool = nullptr;
	std::vector<Image>    images;
	// BufferCache::BdaTableGeneration when the set got the BDA page table: made again once the table grew.
	uint64_t              bda_generation = 0;
	// Proven current in that frame under those registrations, by role (RenderExecutor::Role: [0] also the frame it was
	// made or refused in); used in that frame (by any executor).
	uint64_t validated_frame  = UINT64_MAX; // (role 0's; roles 1..: roles)
	uint64_t resolution_epoch = 0;
	struct Roles1 {
		std::array<uint64_t, Roles - 1> validated_frame, resolution_epoch {};
		Roles1() { validated_frame.fill(UINT64_MAX); }
	};
	TableRoleSide<Roles1> roles;
	// The worker roles' epochs start from role 0's proof (RoleZeroEpoch). At 0 every registration over a view's pages
	// counted, the image's own included, so a set sampling a depth view never validated in a worker (IsSampledDepthCurrent)
	// and every draw with it was a hole: 'table: set validation', ~24-42 a frame settled at three of the owner's spots.
	Roles1& Side() {
		return roles.Get([this](Roles1& side) { side.resolution_epoch.fill(RoleZeroEpoch()); });
	}
	[[nodiscard]] uint64_t RoleZeroEpoch() { return std::atomic_ref<uint64_t>(resolution_epoch).load(std::memory_order_relaxed); }
	uint64_t& ValidatedFrame(uint32_t role) { return role == 0 ? validated_frame : Side().validated_frame[role - 1]; }
	uint64_t& ResolutionEpoch(uint32_t role) { return role == 0 ? resolution_epoch : Side().resolution_epoch[role - 1]; }
	std::atomic<uint64_t>       used_frame {0};
	// The texture cache's flip count (TextureCache::GcTick) and frame (CurrentFrame) when the main executor last checked
	// every image in full, which touches their uses (the LRU, the last use): again within TouchTicks of either (an image
	// unused for 32 frames may go to an overlap, for 600 flips to the collector).
	uint64_t                  touched_tick  = 0;
	uint64_t                  touched_frame = 0;
	static constexpr uint64_t TouchTicks    = 8;
	// The memory its images' views span (from the lowest to the highest byte): registrations elsewhere reach none of
	// its proofs (TableValidateSet). 0, 0: not known yet.
	uint64_t                  span_begin = 0, span_end = 0;
	// TextureCache::ImagesEpoch after the main executor last transited its images (TableTransit) for a draw under
	// transit_state (null: a dispatch): they stay in place while it does not move (UINT64_MAX: not yet).
	uint64_t                  transit_epoch = UINT64_MAX;
	const NativeXprDrawState* transit_state = nullptr;
	// The sum of its images' serials then (KYTY_TABLE_SERIALS; UINT64_MAX: none), and transit_state's capture then.
	uint64_t                  transit_serials  = UINT64_MAX;
	uint64_t                  transit_capture  = 0;
	// The images' ids alone, and whether one is a target of the state it was made under: what the serial sums and
	// TableSetApart read (an Image holds an ImageDesc, lines of it).
	std::vector<ImageId>      image_ids;
	bool                      holds_target = false;
	// An image bound both as a texture and as storage that the shader writes (the deferred decals' G-buffer): with
	// KYTY_DRAW_READ_AFTER_WRITE=1 a draw with the set transits it every time (TableTransit), never in place, ordered
	// after the last draw's write (Image::GetBarriers, issue #315).
	bool                      read_after_write = false;
	// The words select another specialization, or an image no set can hold: the draws take the native path
	// (TableResolveSet tries again after TableImageSet::RetryFrames).
	bool                  refused = false;
	static constexpr uint64_t RetryFrames = 300;
	// KYTY_TABLE_LEARN_PORTABLE=2: a retry that found its program still translating waits for that translation (asked
	// again at every draw until then, it went pending hundreds of times a retry).
	std::shared_ptr<const std::atomic<bool>> retry_wait;
	// A dispatch's words that select another specialization of the pair's program whose table form is the same
	// (TableInterchangeable: its images of other dimensions or formats): that program and its pipeline, which the set
	// is made for (null: the pair's).
	const ShaderRecompiler::IR::CompiledShaderInfo* program  = nullptr;
	const PipelineCache::Pipeline*                  pipeline = nullptr;
};

// Two specializations of a program the table way alike but for their images' and samplers' bindings: the same slots,
// buffers (and their decoded formats), descriptor words, page table reads and shader data. The words TableEvaluate
// reads and the blocks TableBlocks makes for one serve the other.
#if KYTY_CX_INTERCHANGE
static bool TableInterchangeable(const ShaderRecompiler::IR::CompiledShaderInfo& a,
                                 const ShaderRecompiler::IR::CompiledShaderInfo& b) {
	const auto& x = a.bindings;
	const auto& y = b.bindings;
	return a.table_mode && b.table_mode && a.stage == b.stage && a.shader_hash == b.shader_hash &&
	       a.user_data_base == b.user_data_base && a.table_plan == b.table_plan && a.info.buffers == b.info.buffers &&
	       a.info.images.size() == b.info.images.size() && a.info.samplers.size() == b.info.samplers.size() &&
	       x.push_data_start_dword == y.push_data_start_dword &&
	       x.memory_offset_dword == y.memory_offset_dword && x.memory_offset_count == y.memory_offset_count &&
	       x.buffer_word_count == y.buffer_word_count && x.lod_stats_count == y.lod_stats_count &&
	       x.table_block_dwords == y.table_block_dwords && x.user_data_registers == y.user_data_registers;
}
#endif

// A stage's flat form as TableRuns makes it (TablePack packs it into the pair): every operand an index into one of
// three arrays (the stage's user data, its block's slots, the stage's constants).
struct RenderExecutor::TableFlatStage {
	struct Operand {
		uint32_t index = 0;
		uint32_t base  = 2; // 0: user data, 1: slots, 2: constants
	};
	struct Run {
		std::array<Operand, 3> address; // low, high, offset
		int32_t                immediate = 0;
		uint32_t               first = 0, count = 0;
		Operand                records; // a bounded run's (TablePlan::Slot::bounded)
		bool                   bounded = false;
	};
	std::vector<Run>      runs;         // the runs in level order
	std::vector<uint32_t> level_ends;   // the run after each level
	std::vector<Operand>  image_words;  // every image's words, then every sampler's
	std::vector<uint32_t> image_counts;
	std::vector<Operand>  buffer_words; // every buffer's four V# words (TableBlocks)
	std::vector<uint32_t> constants;
	uint32_t              user_words = 0; // the user data it reads: below this
};

struct RenderExecutor::TablePair {
	// Evaluated slots at consecutive addresses (the same base, immediates 4 apart): one read, at the address of the
	// first. Ordered by level: a run whose address is made of slots is read after the runs of those slots.
	struct Run {
		ShaderRecompiler::IR::TablePlan::Slot read;
		uint32_t                              first = 0, count = 0, level = 0;
	};
	uint64_t                                                    vs = 0, ps = 0; // (a dispatch's: its shader, 0)
	// The GS user SGPR the CP writes an indirect draw's start instance into (START_INST_LOC, -1: none): the vertex
	// program is translated for it (ShaderVertexInputInfo::start_instance_sgpr).
	int32_t                                                     start_instance = -1;
	bool                                                        refused  = false;
	const char* special = nullptr; // a dispatch's: the normal path's special path that takes it (refused)
	std::array<const ShaderRecompiler::IR::CompiledShaderInfo*, 2> programs {}; // vertex, pixel (null: inactive)
	// This tree's program cache gives modules: the table programs (vertex, pixel) the pair was stored with, the normal
	// programs' resource plans (an image set's specialization is checked against theirs), the stages' static input key
	// they were compiled for and the code hashes at vs and ps (TableStore).
	std::array<const ShaderModule*, 2>                              tables {};
	std::array<const ShaderRecompiler::IR::ResourcePlan*, 2>        plans {};
	uint64_t                                                        static_key = 0;
	bool                                                            has_pixel  = false;
	std::array<uint64_t, 2>                                         code_hashes {};
	bool                                                            pending = false; // its table programs are being compiled
	std::array<std::vector<Run>, 2>                             runs;
	// TableEvaluate's form of a stage's plan (TableRuns, TablePack): every operand an index into one of three arrays (the
	// stage's user data, its block's slots, the stage's constants), so a draw's evaluation branches on no operand kind.
	// Both stages' arrays in one block, each operand 16 bits (its array in the top two, its index in the low 14), each
	// run 16 bytes: a pair a draw switches to is cold, and its arrays were a dozen allocations of 8-byte operands, as
	// many cold lines on as many pages (TablePrefetchArrays). Same process, 10 rounds: Stonefang standing render 8.94 ->
	// 8.75 ms, Latria 13.47 -> 13.31.
	struct PackedRun {
		std::array<uint16_t, 3>   address {}; // low, high, offset
		uint16_t                  records = NotBounded;
		int32_t                   immediate = 0;
		uint16_t                  first = 0, count = 0;
		static constexpr uint16_t NotBounded = 0xffff;
	};
	struct PackedStage {
		std::span<const PackedRun> runs;
		std::span<const uint16_t>  level_ends, image_words, buffer_words;
		std::span<const uint8_t>   image_counts;
		std::span<const uint32_t>  constants;
		uint32_t                   user_words = 0;
		bool                       valid      = false; // (else an index or a count past its packed width)
	};
	std::array<PackedStage, 2>      packed;
	std::unique_ptr<std::byte[]>    packed_block;
	const std::byte*                packed_at    = nullptr; // (the block's first 64-byte line)
	uint32_t                        packed_bytes = 0;
	std::vector<TableVariant>                                   variants;
	uint32_t                                                    variant_hint = 0; // the one TableTry found last
	uint64_t                                                    store_frame = 0, store_state = 0; // the last request
	std::unordered_map<uint64_t, std::unique_ptr<TableImageSet>, HashedKey> sets; // by T#/S# words
	TableImageSet*                                              last_set = nullptr; // (role 0's; roles 1..: roles)
	// The set before the last, by role (TableSet asks it before the map: a pass's draws of one pair alternate between
	// two materials' sets). Cleared with last_set.
	TableImageSet*                                              previous_set = nullptr;
	// ShaderMapGeneration under which the shaders at vs and ps were proven those the programs were compiled
	// from (the game registers shaders often; code replaced at an address resets the pair), by role.
	uint64_t                                                    generation = UINT64_MAX;
	struct Roles1 {
		std::array<TableImageSet*, Roles - 1> last_set {}, previous_set {};
		std::array<uint64_t, Roles - 1>       generation;
		Roles1() { generation.fill(UINT64_MAX); }
	};
	TableRoleSide<Roles1> roles;
	TableImageSet*& LastSet(uint32_t role) { return role == 0 ? last_set : roles.Get().last_set[role - 1]; }
	TableImageSet*& PreviousSet(uint32_t role) { return role == 0 ? previous_set : roles.Get().previous_set[role - 1]; }
	uint64_t&       Generation(uint32_t role) { return role == 0 ? generation : roles.Get().generation[role - 1]; }
	// Every role's last and previous set dropped (a set replaced or retired).
	void ClearSets() noexcept {
		last_set = previous_set = nullptr;
		if (auto* side = roles.Peek()) {
			side->last_set.fill(nullptr);
			side->previous_set.fill(nullptr);
		}
	}
	// A dispatch's (TableDispatch): its program's input and pipeline, and the normal path's dispatches seen.
	std::unique_ptr<ShaderComputeInputInfo> compute;
#if KYTY_CX_INTERCHANGE
	ShaderProgram                           compute_program;
#endif
	const PipelineCache::Pipeline*          pipeline  = nullptr;
	uint32_t                                sightings = 0;
	// Per stage, the buffers whose V#s were seen in another format than the programs decode (bits by buffer): the
	// programs are compiled again with those buffers' formats decoded at run time (`respecialize`, TableReset).
	std::array<uint32_t, 2> portable {};
	bool                    respecialize = false;
	// A translation or a pipeline compile the pair's next store needs (TableStore, TableDispatch): until it is done,
	// its draws keep the native path without asking for a store (a request a frame each kept the store budget).
	std::shared_ptr<const std::atomic<bool>> waiting;
	// A dispatch's: the pair of the table dispatch that came after this one's the last time (the main executor's):
	// TablePrefetchNext loads its lines while this one is made.
	TablePair* next = nullptr;
	// The frame the main executor last looked it up (TableSweepPairs lets one go that none used for a while).
	uint64_t used_frame = 0;
	// The version of the view the scout evaluates its draws from (Scout::View, TableScoutPublish; 0: none).
	uint64_t scout_version = 0;
};

struct RenderExecutor::TableXpr {
	using Pair    = TablePair;
	using Variant = TableVariant;
	std::unordered_map<uint64_t, Pair, HashedKey> pairs;      // by shader addresses
	std::unordered_map<uint64_t, Pair, HashedKey> dispatches; // by the compute stage's registers (TableDispatchKey)
	// Pairs no draw or dispatch used for PairKeepFrames leave (TableSweepPairs: every PairSweepFrames, past PairsKept):
	// they are keyed by the shaders' addresses, and a world change loaded its shaders elsewhere (~1000 new pairs, ~8 KB
	// each with their runs). Pairs do not move: a pointer stays valid until then, when every one held is dropped.
	static constexpr uint64_t PairSweepFrames = 600, PairKeepFrames = 3600;
	static constexpr size_t   PairsKept       = 2048;
	uint64_t                  pairs_swept     = 0;
	// The pairs found last, direct-mapped by key.
	static constexpr size_t PairCacheEntries = 4096; // (kyty_local_table_mru bit 1: else its first 256 only)
	std::array<std::pair<uint64_t, Pair*>, PairCacheEntries> pair_cache {};
	// The pair TableTry found last, by its key (consecutive draws mostly keep their shaders: no hash of the key, ~0.6% of
	// the render thread at 1-1 standing, nor lookup then).
	std::array<uint64_t, 3> found_key {};
	Pair*                   found_pair = nullptr;
	Pair*                   previous_dispatch = nullptr; // the pair of the last table dispatch (TablePrefetchNext)
	// The same for TableDispatch's dispatches (the main executor's own: its catalog), by TableDispatchWords.
	std::array<uint64_t, 4> found_dispatch_key {};
	Pair*                   found_dispatch = nullptr;
	uint64_t collected  = 0;                           // the frame unused image sets were last dropped
	// Moves when a pair is reset or a set leaves (sets are replaced): an executor reading this catalog drops its last
	// pair, state and set (RenderExecutor::m_catalog) when it moved since it saw it (seen_epoch, its own table's).
	uint64_t epoch = 0, seen_epoch = 0;
	// The next prepared draw of the pair under this state stores (TableStore); the frame's store requests so far.
	Pair*    store       = nullptr;
	uint64_t store_state = 0, store_frame = 0;
	uint32_t store_requests = 0;
	// The draw's T#/S# words (the set's key) and pixel stage LOD statistics words; its runs' addresses.
	TableWords                           words;
	std::vector<uint32_t>                lod_stats;
	std::vector<uint64_t>                addresses;
	// 16 KiB guest pages seen mapped (direct-mapped by page), while the mapping epoch is the same.
	std::array<uint64_t, 256> mapped {};
	uint64_t                  mapping_epoch = UINT64_MAX;
	// The last table draw (TableContinue): its pair, the index of its variant and its draw state.
	Pair*                     last_pair    = nullptr;
	size_t                    last_variant = 0;
	const NativeXprDrawState* last_state   = nullptr;
	const TableImageSet*      last_set     = nullptr; // the set the last emission bound
	// KYTY_TABLE_MEMO: by pair and state key (direct-mapped), what a table draw of them drew with last: the variant's
	// index, the draw state and the image set. Entries hold while memo_epoch does not move: every set retirement
	// (NativeXprRetireSet), state sweep (TableForgetStates), pair sweep and reset moves it.
	struct Memo {
		const Pair*               pair    = nullptr;
		uint64_t                  key     = 0;
		uint64_t                  epoch   = 0;
		const NativeXprDrawState* state   = nullptr;
		TableImageSet*            set     = nullptr;
		uint32_t                  variant = 0;
	};
	static constexpr size_t     MemoEntries = 4096;
	std::array<Memo, MemoEntries> memo {};
	uint64_t                    memo_epoch  = 1;
	TableImageSet*              memo_set    = nullptr; // TableSet's first try for the draw being made
	// KYTY_TABLE_MEMO_VERIFY: memo hits counted (every Nth checked), and whether TableSet checks the memo's set.
	uint64_t                    memo_verify_count = 0;
	bool                        memo_verify       = false;
	[[nodiscard]] Memo& MemoAt(const Pair* pair, uint64_t key) {
		return memo[((reinterpret_cast<uintptr_t>(pair) * 0x9e3779b97f4a7c15ull) ^ (key * 0xc2b2ae3d27d4eb4full)) >> 52u];
	}
	uint64_t                  images_epoch = 0;       // TextureCache::ImagesEpoch after it
	// The game buffer the last arguments came from, and the indices' by their address (each mesh has its own): used
	// again while no registration changed and the guest wrote none of its region (ObtainBuffer then returns the same
	// buffer; it streams CPU-dirty ranges).
	struct HeldBuffer {
		uint64_t begin = 0, end = 0, cpu_epoch = 0, registration = 0;
		Buffer*  buffer = nullptr;
	};
	HeldBuffer                   args;
	std::array<HeldBuffer, 8192> indices;
	// A V#'s buffer as the shaders read it (direct-mapped by the V#'s words, one cache line each): its device address
	// and size in dwords (0: none; clamped to the mappings), while no buffer covering its tracker regions was registered
	// or unregistered since `registration` (the registration epoch it was last proven at: a region stamped later moved
	// a buffer) and the guest wrote none of its range (the tracker regions' epoch, else the range's dirty pages). One
	// whose proof also needs the image starts and the mappings (formatted: an image may start at it; clamped: a mapping
	// may extend it) holds while `special` is the table's special generation (0: no such need). (Buffer registrations
	// anywhere used to drop them all: 3800 resolves a frame walking at 1-1, 1000 standing.)
	// One holding a copy in the table upload ring of a range the guest wrote (RingCopy, made or shared for the main
	// executor) has `registration` RingCopyMark | the observation epoch it is shared in and `cpu_epoch` the range's
	// tracker regions' epoch then: the use takes it as RingCopy's terms allow, without resolving the V# again.
	struct alignas(64) ResolvedBuffer {
		std::array<uint32_t, 4> words {};
		uint32_t                dwords = 0, special = 0;
		vk::DeviceAddress       device = 0;
		uint64_t                address = 0, size = 0, cpu_epoch = 0, registration = 0;
	};
	static constexpr uint64_t RingCopyMark = uint64_t {1} << 63u;
	static constexpr size_t           ResolvedEntries = 65536; // (Scout::ResolvedEntries: the scout's tickets index it)
	std::unique_ptr<ResolvedBuffer[]> resolved = std::make_unique<ResolvedBuffer[]>(ResolvedEntries);
	// A copy in the table upload ring of a small range the guest wrote (TableBlocks reads such a range from a copy,
	// none held: the guest writes its pages unseen), direct-mapped by the range: the main executor's uses share it
	// while no point the guest could learn that GPU work ran at passed (PageManager::ObservationEpoch: until then the
	// guest writes nothing that work reads), in the same submission (`tick`: the ring keeps the copy for it), with
	// the range's tracker regions' epoch unchanged (`cpu_epoch`: it moves when pages become CPU-dirty, when the
	// emulator writes dirty pages, MemoryTracker::NoteHostWrite, and when GPU-written pages are given back) and no
	// page of it GPU-written at the use. A frame's constants are read by many draws: in the Latria spin ~2950 copies,
	// 11.8 MB a frame, before; ~1140 and 4.6 MB with the shared ones.
	struct RingCopy {
		uint64_t          address = 0, size = 0, observation = 0, tick = 0, cpu_epoch = 0;
		vk::DeviceAddress device = 0;
	};
	static constexpr size_t           RingCopies = 4096;
	std::unique_ptr<RingCopy[]>       ring_copies = std::make_unique<RingCopy[]>(RingCopies);
	uint32_t                          special_generation = 1; // moves with the image starts and the mappings
	// Addresses no image starts at, each with the TextureCache::StartEpoch it was proven at (only an address gaining
	// its first image moves that epoch): TableBlocks asks the texture cache (a lock and a map lookup) again only then.
	static constexpr size_t                           NoImageStartEntries = 1024;
	std::array<std::pair<uint64_t, uint64_t>, NoImageStartEntries> no_image_start {};
	uint64_t                          resolved_start = UINT64_MAX, resolved_mapping = UINT64_MAX;
	// Each stage's V#s and their entries (TableGather), gathered for `gathered` (the pair of the draw or dispatch being
	// made, null once TableBlocks took them).
	std::array<std::vector<std::pair<std::array<uint32_t, 4>, ResolvedBuffer*>>, 2> entries;
	const TablePair*                                                                gathered = nullptr;
	// A written V#'s range clamped to the mappings, and (`image_checked`) that no image starts at it, while `special`
	// is the special generation (mappings and image starts as then): obtained as written at every use, its proofs
	// (a memory lock and a texture cache lock and lookup) once (direct-mapped by the words).
	struct WrittenRange {
		std::array<uint32_t, 4> words {};
		uint32_t                special = 0;
		bool                    image_checked = false;
		uint64_t                address = 0, size = 0;
	};
	static constexpr size_t                WrittenEntries = 1024;
	std::array<WrittenRange, WrittenEntries> written;
	// The draw's blocks (per stage): its slots (IR::TablePlan::BlockDwords), then its buffers.
	std::array<std::vector<uint32_t>, 2> blocks;
	// Scratch of TableResolveSet.
	std::array<ShaderRecompiler::IR::ResourceSnapshot, 2> snapshots;
	std::array<ShaderStageRuntime, 2>                     runtimes;
	std::array<PreparedBindings, 2>                       prepared;
	ShaderRecompiler::IR::ResourceSpecialization          specialization;
	// An indirect image's candidates for the draw or dispatch being made (per stage), in the order its set binds them
	// (TableEvaluate: the set's key; TableResolveSet), and the enumeration's scratch.
	std::array<std::vector<ShaderRecompiler::IR::DescriptorValue>, 2> indirect;
#if KYTY_CX_INDIRECT
	ShaderRecompiler::IR::AddressProbeCandidates                      probe;
#endif
};
// KYTY_TABLE_MEMO: a descriptor set left service (NativeXprRetireSet): a memo entry may hold it.
void RenderExecutor::TableMemoRetired() {
	if (m_table_xpr) ++m_table_xpr->memo_epoch;
}
// KYTY_TABLE_MEMO_VERIFY: a memo hit checked; `differ`: 1 the variant, 2 the draw state, 4 the image set the full path
// found were not the memo's.
static void TableMemoChecked(uint32_t differ, uint64_t vs, uint64_t ps, uint64_t key) {
	if (differ == 0) return;
	LiveCounters::Add(LiveCounters::TableMemoMismatch);
	static std::atomic<uint32_t> reports {0};
	if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
		std::printf("TABLE_MEMO_MISMATCH vs=0x%llx ps=0x%llx key=0x%llx variant=%d state=%d set=%d\n",
		            static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
		            static_cast<unsigned long long>(key), (differ & 1u) != 0 ? 1 : 0, (differ & 2u) != 0 ? 1 : 0,
		            (differ & 4u) != 0 ? 1 : 0);
}

#if KYTY_CX_SCOUT
// A draw pair's key as TableTry hashes it (the scout's view directory too).
template <typename Pair>
static uint64_t TableScoutHash(const Pair& pair) {
	const std::array<uint64_t, 3> key {pair.vs, pair.ps, static_cast<uint32_t>(pair.start_instance)};
	return XXH3_64bits(key.data(), sizeof(key));
}
#endif

// The pair's programs and what was made for them leave (its shaders changed, or a V#'s format did:
// TablePair::portable); the next draw or dispatch compiles them again.
void RenderExecutor::TableReset(TablePair& pair) {
#if KYTY_CX_SCOUT
	if (pair.scout_version != 0) Scout::Retire(TableScoutHash(pair), pair.scout_version);
#endif
	auto& table = *m_table_xpr;
	EXIT_IF(Role() != 0);
	++table.memo_epoch;
	for (auto& [words, set]: pair.sets) NativeXprRetireSet(set->pool, set->set);
	if (table.store == &pair) table.store = nullptr;
	if (table.last_pair == &pair) table.last_pair = nullptr;
	table.last_set = nullptr;
	++table.epoch;
	// (The key's start instance SGPR too: without it every later draw of the pair failed TableTry's key check.)
	pair = {.vs = pair.vs, .ps = pair.ps, .start_instance = pair.start_instance, .generation = pair.generation,
	        .roles = std::move(pair.roles), .sightings = pair.sightings, .portable = pair.portable};
	pair.ClearSets();
}

// Draw states left (NativeXprSweepStates): the last table draw's and the image sets' transits under one are forgotten,
// and the executors reading this catalog drop what they hold of it (epoch).
void RenderExecutor::TableForgetStates() {
	auto* table = m_table_xpr.get();
	if (table == nullptr) return;
	++table->memo_epoch;
	table->last_pair  = nullptr;
	table->last_state = nullptr;
	table->last_set   = nullptr;
	++table->epoch;
	for (auto* pairs: {&table->pairs, &table->dispatches})
		for (auto& [key, pair]: *pairs)
			for (auto& [words, set]: pair.sets)
				if (set->transit_state != nullptr) {
					set->transit_state   = nullptr;
					set->transit_epoch   = UINT64_MAX;
					set->transit_serials = UINT64_MAX;
				}
}

// The pairs and dispatches no draw or dispatch looked up for PairKeepFrames leave, their image sets retired. Every
// pointer to a pair goes too: the lookups' (pair_cache, found_*), the last draw's and dispatch's, the store request,
// the gather and the prefetch chain (next). Only the catalog's owner, while no speculation translates or has a result
// unfinished (kyty_local_speculations_live; its next translation drops what it held, ForgetStaleCatalog), and only at
// the start of a table draw or dispatch (no pair is held then).
void RenderExecutor::TableSweepPairs() {
	auto* table = m_table_xpr.get();
	if (table == nullptr || Role() != 0) return;
	const auto frame = m_context.FrameNumber();
	if (frame - table->pairs_swept < TableXpr::PairSweepFrames) return;
	// (Put off while a speculation translates or has a result unfinished: tried at every call until done. Checked again
	// under the lock: a translation counted after it reads the catalog after the sweep, and sees its epoch.)
	if (kyty_local_speculations_live.load(std::memory_order_seq_cst) != 0) return;
	table->pairs_swept = frame;
	if (table->pairs.size() + table->dispatches.size() <= TableXpr::PairsKept) return;
	const auto write = CatalogWrite();
	if (kyty_local_speculations_live.load(std::memory_order_seq_cst) != 0) {
		table->pairs_swept = frame - TableXpr::PairSweepFrames;
		return;
	}
	const size_t before = table->pairs.size() + table->dispatches.size();
	for (auto* pairs: {&table->pairs, &table->dispatches})
		std::erase_if(*pairs, [&](auto& item) {
			auto& pair = item.second;
			// (A translation or compile it waits for keeps it.)
			if (pair.used_frame + TableXpr::PairKeepFrames >= frame || pair.waiting) return false;
			// (KYTY_SCOUT: its view retired too, as TableReset does: else it stays in the directory, its slot lost to others.)
#if KYTY_CX_SCOUT
			if (pair.scout_version != 0) Scout::Retire(TableScoutHash(pair), pair.scout_version);
#endif
			for (auto& [words, set]: pair.sets) NativeXprRetireSet(set->pool, set->set);
			return true;
		});
	const size_t after = table->pairs.size() + table->dispatches.size();
	if (after == before) return;
	++table->memo_epoch;
	table->pair_cache         = {};
	table->found_key          = {};
	table->found_pair         = nullptr;
	table->found_dispatch_key = {};
	table->found_dispatch     = nullptr;
	table->previous_dispatch  = nullptr;
	table->store              = nullptr;
	table->gathered           = nullptr;
	table->last_pair          = nullptr;
	table->last_state         = nullptr;
	table->last_set           = nullptr;
	++table->epoch;
	for (auto* pairs: {&table->pairs, &table->dispatches})
		for (auto& [key, pair]: *pairs) pair.next = nullptr;
	if (SlowLog::HitchThreshold() > 0.0)
		std::printf("[tsc %llu] table pairs %zu -> %zu\n", static_cast<unsigned long long>(__rdtsc()), before, after);
}

RenderExecutor::TableXpr& RenderExecutor::OwnTable() {
	if (!m_table_xpr) m_table_xpr = {new TableXpr {}, [](TableXpr* table) { delete table; }};
	return *m_table_xpr;
}

#if 0 // (the speculation's catalog: not ported)
// What this executor held of another's catalog (its last pair, state and set), dropped when the owner reset a pair or
// let a set go since.
void RenderExecutor::DropStaleCatalog() {
	auto* catalog = CatalogTable();
	auto& table   = OwnTable();
	if (Role() == 0 || catalog == nullptr || catalog->epoch == table.seen_epoch) return;
	table.seen_epoch = catalog->epoch;
	table.last_pair  = nullptr;
	table.last_state = nullptr;
	table.last_set   = nullptr;
	// (Its memo holds the owner's sets and states too: the owner's retirements move only the owner's memo_epoch.)
	++table.memo_epoch;
	table.memo_set = nullptr;
}

// A speculation's executor, before its translation reads the catalog (Speculation::Translate): the catalog's sweeps
// (TableSweepPairs, NativeXprSweepStates) run only between translations, so what it kept from the last one of the
// owner's pairs, draw states and sets is dropped here when the owner's epoch moved since: its table's lookups, last
// draw and memo, and its native emission's last state.
void RenderExecutor::ForgetStaleCatalog() {
	if (Role() == 0) return;
	const auto read    = CatalogRead();
	auto*      catalog = CatalogTable();
	if (catalog == nullptr || m_table_xpr == nullptr || catalog->epoch == m_table_xpr->seen_epoch) return;
	auto& table              = *m_table_xpr;
	table.seen_epoch         = catalog->epoch;
	table.pair_cache         = {};
	table.found_key          = {};
	table.found_pair         = nullptr;
	table.found_dispatch_key = {};
	table.found_dispatch     = nullptr;
	table.previous_dispatch  = nullptr;
	table.store              = nullptr;
	table.gathered           = nullptr;
	table.last_pair          = nullptr;
	table.last_state         = nullptr;
	table.last_set           = nullptr;
	table.memo_set           = nullptr;
	++table.memo_epoch;
	if (m_native_xpr) {
		m_native_xpr->emit           = {};
		m_native_xpr->last_state     = nullptr;
		m_native_xpr->last_state_key = 0;
	}
}

#endif

// A table draw's or dispatch's pair: each draw's pair is another, cold, object and its evaluation
// walks a dozen arrays one after the other (runs, operands, V# words, constants, the last image set's words). Their
// lines are asked for once the pair is known (its own lines first: the vectors' headers), so the misses overlap the
// checks before the evaluation instead of following each other (1-1 spin, per call: table draw path 1.78 -> 1.68 us,
// table dispatch path 2.96 -> 2.86 us).
template <typename Pair>
static void TablePrefetchPair(const Pair& pair) {
	for (size_t line = 0; line < sizeof(Pair); line += 64) __builtin_prefetch(reinterpret_cast<const char*>(&pair) + line);
}
template <typename Pair, typename Set>
static void TablePrefetchArrays(const Pair& pair, const Set* set) {
	const auto lines = [](const auto& items) {
		const auto* at    = reinterpret_cast<const char*>(items.data());
		const auto  bytes = std::min<size_t>(items.size() * sizeof(*items.data()), 256);
		for (size_t offset = 0; offset < bytes; offset += 64) __builtin_prefetch(at + offset);
	};
	for (uint32_t offset = 0; offset < std::min<uint32_t>(pair.packed_bytes, 2048); offset += 64)
		__builtin_prefetch(pair.packed_at + offset);
	for (uint32_t st = 0; st < 2 && pair.programs[st] != nullptr; ++st) {
		lines(pair.programs[st]->table_plan.buffers); // (TableBlocks' plan and resource flags of each V#)
		lines(pair.programs[st]->info.buffers);
	}
	if (set != nullptr) {
		__builtin_prefetch(set);
		lines(set->words);
	}
}

// A frame's dispatches come in much the same order frame after frame (the culling chain's ~1000 of a few hundred
// programs): the pair that followed this one's last dispatch is noted, and the pairs of the next two dispatches expected
// load while this one is made, in two steps (each line a load reads is in by then: the next pair's arrays from its own
// lines, which the dispatch before asked for, and the pair after it, whose lines the next one's arrays need). Their
// lines were cold at each use: TablePrefetchPair asks for them only once the pair is found, ~0.1 us before the first
// read. Census per call, 1-1 standing and Latria (2 boots x 2 rounds each): async-queue table dispatches 1.76 -> 1.70
// and 2.06 -> 2.00 us (their evaluation -9%), graphics-queue ones -4%. (For draws, whose successors vary more, the
// same cost +3% a draw.)
template <typename Pair>
static void TablePrefetchNext(Pair*& previous, Pair& pair) {
	if (previous != nullptr && previous != &pair) previous->next = &pair;
	previous = &pair;
	auto* const next = pair.next;
	if (next == nullptr || next == &pair) return;
	using Set = std::remove_pointer_t<std::remove_cvref_t<decltype(next->last_set)>>;
	TablePrefetchArrays(*next, static_cast<const Set*>(nullptr));
	__builtin_prefetch(next->last_set);
	if (auto* const after = next->next; after != nullptr && after != &pair && after != next) TablePrefetchPair(*after);
}

RenderExecutor::TableResult RenderExecutor::TableTry(CommandBuffer& buffer, std::span<const uint64_t> commands,
                                                     uint64_t index_base, uint64_t index_bytes,
                                                     vk::IndexType index_type, const NativeXprDirectDraw* direct) {
	auto& table = OwnTable();
	DropStaleCatalog();
	TableSweepPairs();
	LiveCensus::Scope census(LiveCensus::DrawPhase, 0x7ab1e, 0); // census: the table path's phases (b)
	auto*       catalog = CatalogTable();
	auto&       cache   = *m_native_xpr;
	const auto& sh      = buffer.GetShaders();
	const auto& vs      = sh.GetVs();
	// The vertex program is the ES stage's (ShaderGetInputInfoVS), without tessellation.
	if (vs.es_regs.data_addr == 0 || vs.ls_regs.data_addr != 0 || vs.hs_regs.data_addr != 0)
		return Spec::Refuse("table: stages"), TableResult::Native;
	const auto frame      = m_context.FrameNumber();
	const auto vs_address = vs.es_regs.data_addr, ps_address = sh.GetPs().ps_regs.data_addr;
	const std::array<uint64_t, 3> pair_key {vs_address, ps_address, static_cast<uint32_t>(TableStartInstance(vs))};
	auto* own   = Role() != 0 ? nullptr : &table.pairs;
	auto* known = own != nullptr && table.found_key == pair_key ? table.found_pair : nullptr;
	if (known == nullptr) {
		const auto pair_hash = XXH3_64bits(pair_key.data(), sizeof(pair_key));
		// (Another executor's catalog: what it learned, none of it made here.)
		const auto found = catalog == nullptr || Role() == 0 ? decltype(catalog->pairs.end()) {} : catalog->pairs.find(pair_hash);
		if (Role() != 0 && (catalog == nullptr || found == catalog->pairs.end())) return Spec::Refuse("table store"), TableResult::Native;
		// (A frame's draws use ~600 pairs at 1-1: 256 entries missed often, each a cold map bucket and node.)
		const auto entries = (kyty_local_table_mru.load(std::memory_order_relaxed) & 1u) != 0 ? TableXpr::PairCacheEntries : 256;
		auto&      cached  = table.pair_cache[pair_hash & (entries - 1)];
		known        = own != nullptr && cached.first == pair_hash ? cached.second : nullptr;
		if (known == nullptr) {
			auto it = own != nullptr ? own->find(pair_hash) : found;
			if (own != nullptr && it == own->end()) {
				const auto write    = CatalogWrite();
				it                  = own->try_emplace(pair_hash).first;
				Spec::Refuse("pair: created");
				auto& fresh         = it->second;
				fresh.vs            = vs_address;
				fresh.ps            = ps_address;
				fresh.start_instance = TableStartInstance(vs);
			}
			known = &it->second;
			if (own != nullptr) cached = {pair_hash, known};
		}
		if (own != nullptr) {
			table.found_key  = pair_key;
			table.found_pair = known;
		}
	}
	auto& pair = *known;
	TablePrefetchPair(pair);
	if (Role() == 0) pair.used_frame = frame;
	if (pair.vs != vs_address || pair.ps != ps_address || pair.start_instance != TableStartInstance(vs))
		return Spec::Refuse("table: pair key"), TableResult::Native;
	if (const auto generation = ShaderMapGeneration(); generation != pair.Generation(Role())) {
		if (pair.programs[0] != nullptr &&
		    (ShaderHashAt(vs_address) != pair.programs[0]->shader_hash ||
		     (pair.programs[1] != nullptr && ShaderHashAt(ps_address) != pair.programs[1]->shader_hash))) {
			if (Role() != 0) return Spec::Refuse("table: shaders changed"), TableResult::Native;
			const auto write = CatalogWrite();
			pair.portable    = {};
			Spec::Refuse("reset: shader hash changed");
			TableReset(pair);
		}
		pair.Generation(Role()) = generation;
	}
	if (pair.respecialize) {
		if (Role() != 0) return Spec::Refuse("table: shaders changed"), TableResult::Native;
		const auto write = CatalogWrite();
		Spec::Refuse("reset: respecialize");
		TableReset(pair);
	}
	if (pair.refused) {
		m_table_try_refused = true;
		return Spec::Refuse("table: pair refused"), TableResult::Native;
	}
	// KYTY_FRAMEGEN: the frame's first G-buffer draw goes the normal way (or a native record's), whose hook reads the
	// camera from its constants (a speculation's thread leaves them all to the GPU thread's).
	if (FrameGen::Enabled() && pair.programs[0] != nullptr &&
	    FrameGen::IsGbufferVertexShader(pair.programs[0]->shader_hash) && (Role() != 0 || FrameGen::WantsCamera()))
		return Spec::Refuse("table: frame generation camera"), TableResult::Native;
	// A store at most once a frame per pair and draw state, within the frame's budget (kyty_local_table_store_budget):
	// while its programs and pipeline compile, the pair's draws take the normal path (Deferred). (Once a frame per pair
	// kept its other states' draws on the native path, storing native records, until a later frame.) Those draws
	// stored native records too, which the table variant replaced a frame or two later: 100-230 a slow frame walking
	// into new parts of Boletarian Palace 1-3 (each traced and sorted, ~3 ms of the frame).
	const auto request_store = [&] {
		if (Spec::Current() != nullptr) return Spec::Refuse("table store"), TableResult::Native;
		if (pair.waiting) {
			if (!pair.waiting->load(std::memory_order_acquire))
				return Spec::Refuse("deferred: programs compiling"), LiveCounters::Add(LiveCounters::TableStoreDeferred), TableResult::Deferred; // (programs compiling)
			pair.waiting.reset();
		}
		if (table.store_frame != frame) {
			table.store_frame    = frame;
			table.store_requests = 0;
		}
		if (pair.store_frame == frame && pair.store_state == cache.state_key)
			return Spec::Refuse("deferred: stored this frame"), LiveCounters::Add(LiveCounters::TableStoreDeferred), TableResult::Deferred;
		// (Past the budget, the others draw from native records meanwhile.)
		if (const auto budget = kyty_local_table_store_budget.load(std::memory_order_relaxed);
		    budget != 0 && table.store_requests >= budget)
			return Spec::Refuse("deferred: store budget"), LiveCounters::Add(LiveCounters::TableStoreDeferred), TableResult::Native;
		++table.store_requests;
		Spec::Refuse("store: requested");
		pair.store_frame  = frame;
		pair.store_state  = cache.state_key;
		table.store       = &pair;
		table.store_state = cache.state_key;
		return TableResult::Store;
	};
	// KYTY_TABLE_MEMO: what this pair and state key drew with last (the main executor's), its variant still that key's.
	const bool memo = Role() == 0 && TableMemoOn();
	if (memo) {
		auto& entry = table.MemoAt(&pair, cache.state_key);
		if (entry.pair == &pair && entry.key == cache.state_key && entry.epoch == table.memo_epoch &&
		    entry.variant < pair.variants.size() && pair.programs[0] != nullptr) {
			auto& memo_variant = pair.variants[entry.variant];
			// KYTY_TABLE_MEMO_VERIFY: every Nth hit, what the full path below finds (the variant by its hint or the
			// find, the draw state by the cache or the states' find; read only) against the entry's. A difference
			// drops the entry and the draw takes the full path.
			bool verify = false, differs = false;
			if (const auto every = TableMemoVerifyEvery();
			    every > 0 && memo_variant.state == cache.state_key && memo_variant.pipeline != nullptr &&
			    !memo_variant.refused && ++table.memo_verify_count % static_cast<uint64_t>(every) == 0) {
				verify           = true;
				const bool hinted = (kyty_local_table_mru.load(std::memory_order_relaxed) & 2u) != 0 &&
				                    pair.variant_hint < pair.variants.size() &&
				                    pair.variants[pair.variant_hint].state == cache.state_key;
				const auto full_variant =
				    hinted ? pair.variant_hint
				           : static_cast<uint32_t>(std::ranges::find(pair.variants, cache.state_key, &TableXpr::Variant::state) -
				                                   pair.variants.begin());
				const NativeXprDrawState* full_state = nullptr;
				if (cache.last_state != nullptr && cache.last_state_key == cache.state_key) {
					full_state = cache.last_state;
				} else {
					const auto& states   = Catalog().NativeXprState().states;
					const auto  state_it = states.find(cache.state_key);
					if (state_it != states.end()) full_state = state_it->second.get();
				}
				LiveCounters::Add(LiveCounters::TableMemoVerified);
				const uint32_t differ = (full_variant != entry.variant ? 1u : 0u) | (full_state != entry.state ? 2u : 0u);
				TableMemoChecked(differ, pair.vs, pair.ps, cache.state_key);
				if (differ != 0) {
					differs     = true;
					entry.epoch = 0;
				}
			}
			if (!differs && memo_variant.state == cache.state_key && memo_variant.pipeline != nullptr &&
			    !memo_variant.refused) {
				const auto& memo_state = *entry.state;
				pair.variant_hint      = entry.variant;
				TablePrefetchArrays(pair, pair.LastSet(Role()));
				if (!NativeXprTargetsCurrent(memo_state))
					return LiveCounters::Add(LiveCounters::TableStoreTargets), request_store();
				LiveCounters::Add(LiveCounters::TableMemoHits);
				table.memo_set     = entry.set;
				table.memo_verify  = verify;
				const auto result  = TableDraw(buffer, pair, memo_variant, memo_state, commands, index_base, index_bytes,
				                               index_type, direct);
				table.memo_set     = nullptr;
				table.memo_verify  = false;
				return result;
			}
		}
	}
	// (The variant found last first: a pair's variants are a cold line each, its draws mostly keep their state.)
	const bool hint    = (kyty_local_table_mru.load(std::memory_order_relaxed) & 2u) != 0 &&
	                  pair.variant_hint < pair.variants.size() && pair.variants[pair.variant_hint].state == cache.state_key;
	const auto variant = hint ? pair.variants.begin() + pair.variant_hint
	                          : std::ranges::find(pair.variants, cache.state_key, &TableXpr::Variant::state);
	if (variant != pair.variants.end()) pair.variant_hint = static_cast<uint32_t>(variant - pair.variants.begin());
	if (variant != pair.variants.end() && variant->refused) return Spec::Refuse("table: variant refused"), TableResult::Native;
	if (pair.programs[0] == nullptr || variant == pair.variants.end() || variant->pipeline == nullptr) {
		Spec::Refuse(pair.programs[0] == nullptr ? (pair.pending ? "wait: programs pending" : "wait: no programs")
		             : variant == pair.variants.end() ? "wait: no variant" : "wait: pipeline null");
		return LiveCounters::Add(LiveCounters::TableStoreVariant), request_store();
	}
	// The draw state (TableStore captures it).
	if (cache.last_state == nullptr || cache.last_state_key != cache.state_key) {
		const auto& states   = Catalog().NativeXprState().states;
		const auto  state_it = states.find(cache.state_key);
		if (state_it == states.end()) return request_store();
		cache.last_state_key = cache.state_key;
		cache.last_state     = state_it->second.get();
	}
	const auto& draw_state = *cache.last_state;
	TablePrefetchArrays(pair, pair.LastSet(Role()));
	if (!NativeXprTargetsCurrent(draw_state)) return LiveCounters::Add(LiveCounters::TableStoreTargets), request_store();
	const auto result = TableDraw(buffer, pair, *variant, draw_state, commands, index_base, index_bytes, index_type, direct);
	if (memo && result == TableResult::Drawn)
		table.MemoAt(&pair, cache.state_key) = {&pair, cache.state_key, table.memo_epoch, &draw_state,
		                                        const_cast<TableImageSet*>(table.last_set),
		                                        static_cast<uint32_t>(variant - pair.variants.begin())};
	return result;
}

bool RenderExecutor::TableContinue(CommandBuffer& buffer, std::span<const uint64_t> commands, uint64_t index_base,
                                   uint64_t index_bytes, vk::IndexType index_type, const NativeXprDirectDraw* direct) {
	if (kyty_local_table_xpr_mode.load(std::memory_order_relaxed) < 2 || !m_table_xpr || !m_native_xpr) return false;
	auto& table = OwnTable();
	Spec::Refuse("continue: tried");
	if (table.last_pair == nullptr) return Spec::Refuse("continue: no last pair"), false;
	if (buffer.IsInvalid() || index_base == 0 || index_bytes == 0 || commands.size() > 64)
		return Spec::Refuse("continue: draw shape"), false;
	if (table.last_pair->Generation(Role()) != ShaderMapGeneration())
		return Spec::Refuse("continue: shader map generation"), false;
	if (table.last_pair->start_instance != TableStartInstance(buffer.GetShaders().GetVs()))
		return Spec::Refuse(g_table_offsets_key < 0 || table.last_pair->start_instance < 0 ? "continue: direct/indirect switch"
		                                                                                   : "continue: other offsets"), false;
	auto&       variant = table.last_pair->variants[table.last_variant];
	const auto& emit    = m_native_xpr->emit;
	if (emit.generation != m_context.GetCommandScheduler().GetDynamicState().Generation())
		return Spec::Refuse("continue: emit generation"), false;
	if (table.last_state == nullptr || emit.state != table.last_state) return Spec::Refuse("continue: emit state"), false;
	if (emit.pipeline != variant.pipeline) return Spec::Refuse("continue: emit pipeline"), false;
	// (The render lock is the caller's: TableXprTry.)
	if (!NativeXprTargetsCurrent(*table.last_state)) return Spec::Refuse("continue: targets"), false;
	if (TableDraw(buffer, *table.last_pair, variant, *table.last_state, commands, index_base, index_bytes, index_type,
	              direct) != TableResult::Drawn)
		return Spec::Refuse("continue: draw refused"), false;
	LiveCounters::Add(LiveCounters::TableContinued, std::max<size_t>(commands.size(), 1));
	return true;
}

// The image set of the T#/S# words (`words`) of a draw (under `variant`) or a dispatch (`variant` null): its
// resources as the normal path evaluates them, which must select the pair's table mode programs (the same
// specializations), and its images and samplers as the normal path resolves and acquires them (PrepareBindingsInto,
// RebindImages), in a descriptor set with the page table and the fault buffer. Refused for another specialization,
// or when an image cannot be held in a set (a target the shader cannot read, or writes as storage).
// KYTY_TABLE_LEARN_PORTABLE (live table_learn_portable; mode 4: 2, else 0): 1, a set refused for another
// specialization that only a buffer's V# format makes (one pair's draws with an 8 and an 8_8_8_8 vertex stream) learns
// that buffer as portable instead; 2, also a retry still pending waits for its translation.
static int64_t TableLearnPortable() {
	static auto& value = LiveSwitch::Find("table_learn_portable", 0);
	return Spec::On() ? value.load(std::memory_order_relaxed) : 0;
}

RenderExecutor::TableImageSet* RenderExecutor::TableResolveSet(CommandBuffer& buffer, TablePair& pair,
                                                               const TableVariant*       variant,
                                                               const NativeXprDrawState* draw_state,
                                                               std::span<const uint32_t> words) {
	(void)buffer;
	if (Spec::Current() != nullptr) return Spec::Refuse("table image set"), nullptr;
	auto       set      = std::make_unique<TableImageSet>();
	auto&      table    = OwnTable();
	auto&      textures = m_context.GetTextureCache();
	// (Before the lookups: a registration they make over a depth image's pages is one IsSampledDepthCurrent sees.)
	const auto epoch    = textures.ResolutionEpoch();
	auto&      buffers  = m_context.GetBufferCache();
	const auto stages   = pair.programs[1] != nullptr ? 2u : 1u;
	set->words.assign(words.begin(), words.end());
	set->validated_frame = m_context.FrameNumber();
	const auto publish = [&] {
		const auto write = CatalogWrite();
		auto&      slot  = pair.sets[XXH3_64bits(words.data(), words.size() * sizeof(uint32_t))];
		if (slot) {
			NativeXprRetireSet(slot->pool, slot->set);
			pair.ClearSets();
			++m_table_xpr->epoch;
		}
		slot = std::move(set);
		return slot.get();
	};
	const auto refuse = [&](const char* reason) {
		static std::atomic<uint32_t> reports[2] {}; // (dispatches, draws)
		if (reports[pair.ps != 0 ? 1 : 0].fetch_add(1, std::memory_order_relaxed) < 32)
			std::printf("Table: VS 0x%llx PS 0x%llx image set refused: %s\n", static_cast<unsigned long long>(pair.vs),
			            static_cast<unsigned long long>(pair.ps), reason);
		set->refused = true;
		return publish();
	};
	// Each stage's resources from the evaluation: its V#s (TableGather), T#s and S#s (the set's words), as the
	// normal path evaluates them. (His version asks the program cache for the pair's programs under these resources
	// (GetGraphicsPrograms in table mode); here the pair's table programs were compiled by TableStore and the
	// resources' specialization is compared with theirs.)
	size_t at = 0;
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& plan     = pair.programs[st]->table_plan;
		auto&       snapshot = table.snapshots[st];
		snapshot.buffers.resize(plan.buffers.size());
		for (size_t i = 0; i < plan.buffers.size(); ++i) {
			snapshot.buffers[i]             = {};
			snapshot.buffers[i].dword_count = 4;
			std::copy_n(table.entries[st][i].first.begin(), 4, snapshot.buffers[i].dwords.begin());
		}
		const auto take = [&](uint32_t count, ShaderRecompiler::IR::DescriptorValue& value) {
			value             = {};
			value.dword_count = count;
			std::copy_n(words.begin() + static_cast<std::ptrdiff_t>(at), count, value.dwords.begin());
			at += count;
		};
		snapshot.images.resize(plan.images.size());
		for (size_t i = 0; i < plan.images.size(); ++i) take(plan.images[i].count, snapshot.images[i]);
		snapshot.samplers.resize(plan.samplers.size());
		for (size_t i = 0; i < plan.samplers.size(); ++i) take(plan.samplers[i].count, snapshot.samplers[i]);
		snapshot.flattened_srt.clear();
		// (PrepareBindings copies the user data registers into shader data the set does not use.)
		uint32_t user_words = 0;
		for (const auto reg: pair.programs[st]->info.user_data_registers)
			user_words = std::max<uint32_t>(user_words, reg - pair.programs[st]->user_data_base + 1);
		snapshot.user_data.assign(user_words, 0u);
		snapshot.specialization_reads.clear();
		snapshot.image_tables.clear();
		snapshot.uniform_fill = {};
		// (Its images' and samplers' specialization must be the program's: its buffers' are checked at every use.)
		auto& specialization = table.specialization;
		if (!m_context.GetPipelineCache().SpecializeDirectResources(*pair.plans[st], snapshot, specialization))
			return refuse("resources that do not specialize");
		if (specialization.images != pair.tables[st]->specialization.images ||
		    specialization.samplers != pair.tables[st]->specialization.samplers)
			return refuse(st == 0 ? "another specialization (VS)" : "another specialization (PS)");
		table.runtimes[st] = {.module = pair.tables[st], .resources = &snapshot};
	}
	// The normal path's lookups and acquisitions, and each image's layout and access as its CommitBindings gives them:
	// a target of the draw state in its attachment layout and access (NativeXprEmitTargets moves it there), one bound
	// both as a texture and as storage (force_general: the deferred decals' G-buffer) wholly in General, storage in
	// General, else read-only.
	struct Unbind {
		RenderExecutor& executor;
		~Unbind() { executor.ResetBindings(); }
	} unbind {*this};
	for (uint32_t st = 0; st < stages; ++st) PrepareBindings(table.runtimes[st], table.prepared[st]);
	for (uint32_t st = 0; st < stages; ++st) RebindImages(table.prepared[st]);
	std::array<std::vector<TableImageSet::Image>, 2> deps;
	for (uint32_t st = 0; st < stages; ++st) {
		deps[st].reserve(table.prepared[st].images.size()); // (no regrowth: new sets come in bursts)
		for (auto& binding: table.prepared[st].images) {
			if (!binding.image_id) continue; // (a null image: bound as the normal path binds it)
			auto&       image   = textures.GetImage(binding.image_id);
			const bool  storage = binding.desc.type == TextureCache::BindingType::Storage;
			const auto  found   = draw_state == nullptr ? decltype(draw_state->targets.end()) {}
			                                            : std::ranges::find(draw_state->targets, binding.image_id,
			                                                                &NativeXprDrawState::Target::id);
			const auto* target  = draw_state == nullptr || found == draw_state->targets.end() ? nullptr : &*found;
			const auto& view    = binding.desc.view_info;
			// KYTY_TABLE_TARGET_READ: a target sampled in a layout that lets the shader read what it samples (the checks of
			// the normal path's CommitBindings) is held with the shader access added, as that path transits it.
			const bool target_read = [&] {
				if (target == nullptr || (target->access & vk::AccessFlagBits2::eShaderRead) ||
				    kyty_local_table_target_read.load(std::memory_order_relaxed) == 0)
					return false;
				const auto layout = target->layout;
				// (A colour target bound as storage too, in General: the deferred decals' G-buffer, as CommitBindings.)
				if (storage) return !image.info.IsDepth() && layout == vk::ImageLayout::eGeneral;
				if (layout == vk::ImageLayout::eGeneral) return true;
				const bool pixel_feedback = layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT && stages == 2 && st == 1;
				if (!image.info.IsDepth()) return pixel_feedback;
				const auto aspect     = binding.desc.view_info.aspect;
				const bool depth_read = pixel_feedback || layout == vk::ImageLayout::eDepthReadOnlyOptimal ||
				                        layout == vk::ImageLayout::eDepthStencilReadOnlyOptimal ||
				                        layout == vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
				const bool stencil_read = layout == vk::ImageLayout::eStencilReadOnlyOptimal ||
				                          layout == vk::ImageLayout::eDepthStencilReadOnlyOptimal ||
				                          layout == vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal;
				return (!(aspect & vk::ImageAspectFlagBits::eDepth) || depth_read) &&
				       (!(aspect & vk::ImageAspectFlagBits::eStencil) || stencil_read);
			}();
			if (image.info.data.Empty()) return refuse("an image without data");
			if (target != nullptr && !target_read && (storage || !(target->access & vk::AccessFlagBits2::eShaderRead)))
				return refuse("a target the shader reads");
			if ((storage || (image.binding.force_general && image.binding.shader_write)) && !image.info.IsTiled())
				return refuse("a linear image the shader writes");
			auto& dep = deps[st].emplace_back();
			dep       = {.id     = binding.image_id,
			             .serial = image.serial,
			             .layout = vk::ImageLayout::eUndefined,
			             .range  = {view.base_level, view.level_count, view.base_layer, view.layer_count},
			             .desc   = binding.desc};
			if (target != nullptr) {
				dep.layout      = target->layout;
				dep.target_read = target_read ? vk::AccessFlagBits2::eShaderRead |
				                                    (storage || image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite
				                                                                           : vk::AccessFlags2 {})
				                              : vk::AccessFlags2 {};
				dep.access      = target->access | dep.target_read;
				dep.target      = true;
			} else if (image.binding.force_general && !image.info.IsDepth()) {
				dep.layout  = vk::ImageLayout::eGeneral;
				dep.access  = vk::AccessFlagBits2::eShaderRead |
				              (image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlags2 {});
				dep.storage = image.binding.shader_write;
				dep.whole   = true;
			} else if (storage) {
				dep.layout  = vk::ImageLayout::eGeneral;
				dep.access  = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
				dep.storage = true;
			} else {
				dep.layout = image.info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
				                                  : vk::ImageLayout::eShaderReadOnlyOptimal;
			}
			binding.layout = dep.layout;
		}
	}
	// The set: per stage its bindings (images, samplers, LOD statistics, page table, fault buffer), as CommitBindings
	// writes them.
	using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
	struct Pending {
		vk::WriteDescriptorSet write;
		size_t                 start = 0;
		bool                   image = false;
	};
	std::vector<Pending>                    pending;
	std::array<vk::DescriptorBufferInfo, 4> buffer_infos {};
	size_t                                  buffer_count = 0;
	std::vector<vk::DescriptorImageInfo>    image_infos;
	set->bda_generation = BufferCache::BdaTableGeneration(); // (read before the table: BufferCache::GrowBdaTable)
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& program  = *pair.programs[st];
		const auto& prepared = table.prepared[st];
		std::array<size_t, ShaderRecompiler::IR::ImageBindingCount> image_starts {};
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto kind_index = std::countr_zero(mask);
			const auto kind       = static_cast<Kind>(kind_index);
			Pending    item;
			item.write.dstBinding      = ShaderRecompiler::IR::NativeBinding(program.stage, kind);
			item.write.descriptorType  = NativeDescriptorType(kind);
			item.write.descriptorCount = program.bindings.descriptor_counts[kind_index];
			if (ShaderRecompiler::IR::ImageBindingResourceClass(kind) != ShaderRecompiler::IR::ImageResourceClass::None) {
				item.image = true;
				item.start = image_infos.size();
				image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] = item.start;
				image_infos.resize(image_infos.size() + item.write.descriptorCount);
			} else if (kind == Kind::Samplers) {
				item.image = true;
				item.start = image_infos.size();
				if (prepared.samplers.size() != item.write.descriptorCount) return refuse("sampler count");
				for (const auto sampler: prepared.samplers) {
					if (sampler == nullptr) return refuse("a null sampler");
					image_infos.emplace_back(sampler, nullptr, vk::ImageLayout::eUndefined);
				}
			} else if (kind == Kind::MipStats && buffer_count < buffer_infos.size()) {
				const auto& counters         = m_context.GetLodStats().CounterBuffer();
				item.start                   = buffer_count;
				buffer_infos[buffer_count++] = {counters.Handle(), 0, counters.Size()};
			} else if ((kind == Kind::BdaPagetable || kind == Kind::FaultBuffer) && buffer_count < buffer_infos.size()) {
				const auto* owner = kind == Kind::BdaPagetable ? buffers.GetBdaPageTableBuffer() : buffers.GetFaultBuffer();
				item.start        = buffer_count;
				buffer_infos[buffer_count++] = {owner->Handle(), 0, owner->Size()};
			} else {
				return refuse("a binding table mode does not have");
			}
			pending.push_back(item);
		}
		if (prepared.images.size() != program.info.images.size()) return refuse("image count");
		for (uint32_t i = 0; i < program.info.images.size(); ++i) {
			const auto& resource = program.info.images[i];
			const auto& texture  = prepared.images[i];
			if (texture.image_id && resource.mip_count != (texture.mip_views.empty() ? 1u : texture.mip_views.size()))
				return refuse("image mips");
			const auto kind  = *ShaderRecompiler::IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] + resource.descriptor_index;
			for (uint32_t mip = 0; mip < resource.mip_count; ++mip) {
				if (start + mip >= image_infos.size()) return refuse("image slot");
				image_infos[start + mip] = MakeImageInfo(texture, mip);
			}
		}
		if (st == 0) set->images.reserve(deps[0].size() + deps[1].size());
		for (const auto& dep: deps[st])
			if (dep.layout != vk::ImageLayout::eUndefined) {
				set->images.push_back(dep);
				set->image_ids.push_back(dep.id);
				set->holds_target |= dep.target;
				set->read_after_write |= dep.whole && dep.storage;
				// (Its view's metadata, which may not be the image's own: KYTY_TABLE_SERIALS.)
				if (dep.desc.info.metadata.kind != ImageMetadataKind::None)
					textures.NoteMetaUse(dep.desc.info.metadata.range.address, dep.id.index);
			}
	}
	set->set = NativeXprAllocateSet((variant != nullptr ? variant->pipeline : pair.pipeline)->descriptor_set_layout, set->pool);
	if (!set->set) return refuse("no descriptor set");
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(pending.size());
	for (auto& item: pending) {
		item.write.dstSet = set->set;
		if (item.image) item.write.pImageInfo = image_infos.data() + item.start;
		else item.write.pBufferInfo = buffer_infos.data() + item.start;
		writes.push_back(item.write);
	}
	if (!writes.empty())
		m_context.GetGraphics().device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
		                                                    nullptr);
	set->resolution_epoch = epoch;
	LiveCounters::Add(LiveCounters::TableEvaluations);
	return publish();
}

// What FindTexture uploads before an image is used with `desc`: guest bytes the CPU wrote, a GPU buffer write over them,
// or one over its stencil plane (of a sampled view, only into its levels: TextureCache::RefreshPending).
static bool TableRefreshPending(const Image& image, const TextureCache::ImageDesc& desc) {
	(void)desc;
	return TextureCache::RefreshPending(image);
}

// A set's images are the same objects, registered and current (as NativeXprValidateResources proves a record's):
// once a frame, and again when registrations moved. The main executor checks a use it proved again only when its
// image's validity moved or a registration stamped the view's pages since (1-1: ~1500 sets a frame, ~0.5 ms, nearly
// none changed; the registrations, every frame, one depth surface's memory taken as a color target and back), and
// every use in full once in TouchTicks flips or frames, which touches it (KYTY_TABLE_SET_PROOFS: 0 checks all of
// them in full, 2 does and counts the uses a proof held for that did not pass).
bool RenderExecutor::TableValidateSet(TableImageSet& set) {
	auto&      textures   = m_context.GetTextureCache();
	const auto frame      = m_context.FrameNumber();
	const auto resolution = textures.ResolutionEpoch();
	if (set.bda_generation != BufferCache::BdaTableGeneration()) return false; // (the page table grew since)
	// (A worker's proof is never older than role 0's latest: the registrations since role 0's are what can have moved
	// the views; its own stays where its last pass left it otherwise, and a registration after it failed it for good.)
	if (Role() != 0) {
		auto& mine = set.ResolutionEpoch(Role());
		mine        = std::max(mine, set.RoleZeroEpoch());
	}
	if (set.ValidatedFrame(Role()) == frame && set.ResolutionEpoch(Role()) == resolution) return true;
	LiveCensus::Scope census(LiveCensus::DrawPhase, 0x7ab1e, 7);
	const auto mode  = kyty_local_table_set_proofs.load(std::memory_order_relaxed);
	const bool own   = Role() == 0 && Spec::Current() == nullptr && mode != 0;
	const bool full  = !own || textures.GcTick() >= set.touched_tick + TableImageSet::TouchTicks ||
	                  textures.CurrentFrame() >= set.touched_frame + TableImageSet::TouchTicks ||
	                  m_context.GetCommandScheduler().Current().IsInvalid();
	// Registrations since: their pages say which proofs they reach (none, when the log of the latest has none over the
	// views' span: the game moves registrations every frame somewhere).
	bool moved = set.ResolutionEpoch(Role()) != resolution;
	if (moved && own) {
		if (set.span_end == 0) {
			set.span_begin = UINT64_MAX;
			for (const auto& dep: set.images) {
				set.span_begin = std::min(set.span_begin, dep.desc.info.data.address);
				set.span_end   = std::max(set.span_end, dep.desc.info.data.End());
			}
		}
		bool known = false;
		moved      = set.span_end <= set.span_begin ||
		        textures.RegistrationsOverlapSince(set.span_begin, set.span_end - set.span_begin, set.resolution_epoch, known) ||
		        !known;
	}
	if (!full)
		for (const auto& dep: set.images) textures.m_slot_images.prefetch(dep.id);
	for (auto& dep: set.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial) return false;
		const bool held = !full && dep.proven && TableValidity(*image) == dep.validity &&
		                  (!moved || !textures.RegistrationsSince(dep.desc.info.data.address, dep.desc.info.data.size,
		                                                          set.resolution_epoch));
		if (held && mode != 2) continue;
		const auto refuse = [&] {
			if (held) {
				LiveCounters::Add(LiveCounters::TableProofMismatches);
				static std::atomic<uint32_t> reports {0};
				if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
					std::printf("Table: proven image use failed: addr=0x%llx size=0x%llx serial=%llu validity=%llu\n",
					            static_cast<unsigned long long>(dep.desc.info.data.address),
					            static_cast<unsigned long long>(dep.desc.info.data.size),
					            static_cast<unsigned long long>(dep.serial), static_cast<unsigned long long>(dep.validity));
			}
			return false;
		};
		// The guest rewrote it (a texture streaming in), or a GPU buffer write its bytes or its stencil plane: upload it
		// as the normal path's FindTexture does.
		if (TableRefreshPending(*image, dep.desc)) {
			if (held) (void)refuse();
			if (Spec::Current() != nullptr) return Spec::Refuse("texture upload"), false;
			if (!image->registered || image->depth_id || image->binding.needs_rebind) return false;
			(void)textures.FindTexture(dep.id, dep.desc);
			image = textures.m_slot_images.try_get(dep.id);
			if (image == nullptr || image->serial != dep.serial || TableRefreshPending(*image, dep.desc)) return false;
		}
		if (dep.target) {
			if (!image->registered || image->binding.needs_rebind) return refuse();
			(void)textures.GetImage(dep.id); // LRU
			if (own) {
				dep.proven   = true;
				dep.validity = TableValidity(*image);
			}
			continue;
		}
		// (And its LRU, as FindTexture's. A DCC image written as storage, a downsampling chain's levels: TableTransit
		// checks its pending clears at every use, as a sampled one's.) A depth image sampled as a texture (the scene's
		// depth and stencil read by lighting and post passes: at 1-1 their sets were made again every other frame) is
		// found by other rules than a texture's: the proof is that nothing was registered over its view since the set
		// was last proven (or made).
		bool sampled = textures.IsSampledImageCurrent(dep.id, dep.desc, true);
		bool current = sampled || textures.IsSampledDepthCurrent(dep.id, dep.desc, set.ResolutionEpoch(Role()));
		const bool texture = dep.desc.type == TextureCache::BindingType::Texture;
		if (!current && own && (texture || dep.desc.type == TextureCache::BindingType::Storage) &&
		    !m_context.GetCommandScheduler().Current().IsInvalid()) {
			// A surface the game uses as depth and as colour in turn (0x28a010000 at 1-1 and Latria, twice a frame): the
			// form a use leaves is parked (TextureCache::ParkImage), registrations over it move every frame, and the sets
			// using it failed here at every use (made again, 30-70 us each, the dispatch on the normal path meanwhile). The
			// normal path's lookup takes the set's image back (TakeParkedImage: the same id and serial, its views kept, the
			// other form's bytes copied in): the set then binds what that lookup and its acquisition bind. (The lookup is
			// the proof for a depth-tiled view too, which IsSampledImageCurrent declines: the surface's storage use.)
			// (A view the set's lookup moved into a larger image, its first layer in that image (on AMD GPUs a texture in the
			// layers a texture pool's views hold), is no request of its own: looked up again it ended the emulator (issue
			// #102, an RX 9070 XT). Its image is the one the set holds, and the checks below are its proof.)
			auto       desc  = dep.desc;
			const bool taken = !TextureCache::ViewFitsImage(desc) ||
			                   (textures.FindImage(desc) == dep.id && desc.view_info == dep.desc.view_info);
			image            = textures.m_slot_images.try_get(dep.id); // (the lookup may have moved the slots)
			if (taken && image != nullptr && image->serial == dep.serial && image->registered && !image->depth_id &&
			    !image->binding.needs_rebind) {
				if (texture && (dep.desc.info.IsDepth() || image->info.IsDepth())) {
					current = textures.IsSampledDepthCurrent(dep.id, dep.desc, textures.ResolutionEpoch());
				} else {
					(void)textures.FindTexture(dep.id, dep.desc);
					sampled = textures.IsSampledImageCurrent(dep.id, dep.desc, true);
					current = sampled || (!dep.desc.info.HasMetadata() && !dep.desc.info.HasStencil());
				}
			}
		}
		if (!current) {
			return refuse();
		}
		// (A DCC image's surface and a depth image's view are proven otherwise: checked at every validation.)
		if (own) {
			dep.proven   = sampled && dep.desc.info.metadata.kind != ImageMetadataKind::Dcc;
			dep.validity = TableValidity(*image);
		}
	}
	// (The epoch before the checks: a registration they made, FindTexture's, stamps pages a later check looks at.)
	set.ValidatedFrame(Role()) = frame;
	if (Role() == 0)
		std::atomic_ref<uint64_t>(set.resolution_epoch).store(resolution, std::memory_order_relaxed); // (workers read it)
	else
		set.ResolutionEpoch(Role()) = resolution;
	if (own && full) {
		set.touched_tick  = textures.GcTick();
		set.touched_frame = textures.CurrentFrame();
	}
	return true;
}

// An operand of the stage's code address (a V# of constants the code embeds), and its word: the low or high dword of
// the pair's code address (the stage's registers named it) plus the operand's offset.
static bool TableCodeOperand(const ShaderRecompiler::IR::TablePlan::Operand& operand) {
	using Kind = ShaderRecompiler::IR::TablePlan::Operand::Kind;
	return operand.kind == Kind::ShaderBaseLow || operand.kind == Kind::ShaderBaseHigh;
}
static uint32_t TableCodeWord(const ShaderRecompiler::IR::TablePlan::Operand& operand, uint64_t code) {
	const auto address = code + operand.value;
	return operand.kind == ShaderRecompiler::IR::TablePlan::Operand::Kind::ShaderBaseLow ? static_cast<uint32_t>(address)
	                                                                                    : static_cast<uint32_t>(address >> 32u);
}

// The slots of the pair's stages, read into their blocks where the plans address them from the user data and lower
// slots (a pointer read from memory may hold anything: what is read through it must be mapped), and the T#/S# words
// they make (TableXpr::words, the set's key) and the pixel stage's LOD statistics words. False: something unmapped.
bool RenderExecutor::TableEvaluate(TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data) {
	auto&      table     = OwnTable();
	const auto stages    = pair.programs[1] != nullptr ? 2u : 1u;
	if (const auto epoch = m_context.MappedRangesVersion(); epoch != table.mapping_epoch) {
		table.mapped.fill(UINT64_MAX);
		table.mapping_epoch = epoch;
	}
	using Kind  = ShaderRecompiler::IR::TablePlan::Operand::Kind;
	auto& words = table.words;
	words.clear();
	table.lod_stats.clear();
	// His NativeXprReader: a run inside one page that no read watcher can hold (the granule filter, then the exact
	// answer) is read in place, without the backing view's lookup; the rest as TableReadRun.
	struct Reader {
		RenderContext&               context;
		const std::atomic<uint64_t>* granules = context.ReadGranules();
		uint64_t                     page     = UINT64_MAX;
		bool                         direct   = false;
		bool                         Read(uint64_t address, uint32_t count, uint32_t* out) {
            const uint64_t bytes = uint64_t {count} * 4u;
            const uint64_t first = address >> 12u;
            if ((address & 3u) == 0 && bytes != 0 && first == (address + bytes - 1u) >> 12u) {
                if (first != page) {
                    page               = first;
                    const auto granule = first >> (PageManager::READ_GRANULE_BITS - 12u);
                    direct             = (address < TRACKER_ADDRESS_SIZE &&
                              ((granules[granule / 64u].load(std::memory_order_acquire) >> (granule % 64u)) & 1u) == 0) ||
                             !context.HasReadWatchers(first << 12u, 4096);
                }
                if (direct) {
                    const auto* from = reinterpret_cast<const volatile uint32_t*>(address);
                    for (uint32_t i = 0; i < count; ++i) out[i] = from[i];
                    return true;
                }
            }
            return TableReadRun(address, count, out);
		}
	} reader {m_context};
	// A run's address, its pages proven mapped and its lines loading.
	// (A bounded run, read through a V#: within its range, NUM_RECORDS bytes or NUM_RECORDS strides, as the normal
	// path's evaluation, which fails past it: so does this one.)
	const auto locate = [&](uint64_t low, uint64_t high, uint32_t offset, int32_t immediate, uint32_t count,
	                        uint64_t& out, bool bounded = false, uint32_t records = 0) {
		if (bounded) {
			const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size   = stride == 0 ? uint64_t {records} : uint64_t {stride} * records;
			if (immediate < 0 || static_cast<uint64_t>(immediate) + count * 4ull > size) return false;
		}
		const auto address = ((low | high << 32u) & 0x0000fffffffffffcull) + (offset & ~3u) +
		                     static_cast<uint64_t>(int64_t {immediate});
		for (auto page = address >> 14u; page <= (address + count * 4ull - 1) >> 14u; ++page) {
			auto& seen = table.mapped[page & 255u];
			if (seen == page) continue;
			if (!Libs::LibKernel::Memory::IsFullyMapped(page << 14u, 16384)) return false;
			seen = page;
		}
		for (auto line = address & ~63ull; line < address + count * 4ull; line += 64)
			__builtin_prefetch(reinterpret_cast<const void*>(line));
		out = address;
		return true;
	};
	// The LOD statistics fields of the T#s (the game reassigns the counters every frame) reach only the shader data
	// (as RebindBuffers computes it), not the set.
	// GET_LOD_STATS: the counting field of an image whose T# the draw supplies, as this tree's shader data holds it
	// (LodStatsReport::ImageField, as RebindBuffers computes it; his encoding differs).
	const auto lod_stats = [&](const std::array<uint32_t, 8>& image, uint32_t count) {
		table.lod_stats.push_back(count >= 8u ? LodStatsReport::ImageField(image.data()) : LodStatsReport::NoCounterFlag);
	};
	// A stage from its plan's operands: for user data the flat form cannot index (fewer words than it reads, or more
	// than 64: one beyond them reads 0 here, the flat form's constant 0 stands for those of 64 and up).
	const auto by_operands = [&](uint32_t st) {
		const auto& program = *pair.programs[st];
		const auto& plan    = program.table_plan;
		auto&       slots   = table.blocks[st];
		const auto  value   = [&](const ShaderRecompiler::IR::TablePlan::Operand& operand) -> uint32_t {
			if (operand.kind == Kind::Immediate) return operand.value;
			if (operand.kind == Kind::Slot) return slots[operand.value];
			if (TableCodeOperand(operand)) return TableCodeWord(operand, st == 0 ? pair.vs : pair.ps);
			const auto index = operand.value - program.user_data_base;
			return index < user_data[st].size() ? user_data[st][index] : 0;
		};
		const auto& runs = pair.runs[st];
		for (size_t begin = 0, end = 0; begin < runs.size(); begin = end) {
			for (end = begin; end < runs.size() && runs[end].level == runs[begin].level; ++end) {
				const auto& run = runs[end];
				if (!locate(value(run.read.low), value(run.read.high), value(run.read.offset), run.read.immediate, run.count,
				            table.addresses[end], run.read.bounded, run.read.bounded ? value(run.read.records) : 0))
					return false;
			}
			for (size_t i = begin; i < end; ++i)
				if (!reader.Read(table.addresses[i], runs[i].count, slots.data() + runs[i].first)) return false;
		}
		for (size_t i = 0; i < plan.images.size(); ++i) {
			const auto& descriptor = plan.images[i];
			std::array<uint32_t, 8> image {};
			for (uint32_t w = 0; w < descriptor.count; ++w) {
				image[w] = value(descriptor.words[w]);
				words.push_back(image[w] & ~NativeXprLodStatsMask[w]);
			}
			if (i < program.bindings.mip_stats_count) lod_stats(image, descriptor.count);
		}
		for (const auto& descriptor: plan.samplers)
			for (uint32_t w = 0; w < descriptor.count; ++w) words.push_back(value(descriptor.words[w]));
		return true;
	};
	// An address probe's indirect image (IR::TablePlan::IndirectImage): its candidates as resource materialization
	// enumerates them (the same code and readers), in an order of their own (key 0's first, the rest by their words:
	// one set whatever order the records name them in), their count and words in the set's key, the key mapping (by
	// key) in the block. False: the normal path takes it (a table not readable, more candidates or keys than room).
#if KYTY_CX_INDIRECT
	const auto indirect = [&](uint32_t st) {
		const auto& program = *pair.programs[st];
		auto&       out     = table.indirect[st];
		out.clear();
		if (program.table_plan.indirect.empty()) return true;
		thread_local const auto runtime = [] {
			ShaderRecompiler::IR::SrtRuntime value;
			MaterializationReaders(value);
			return value;
		}();
		const auto& slots   = table.blocks[st];
		const auto  operand = [&](const ShaderRecompiler::IR::TablePlan::Operand& value) -> uint32_t {
			if (value.kind == Kind::Immediate) return value.value;
			if (value.kind == Kind::Slot) return slots[value.value];
			if (TableCodeOperand(value)) return TableCodeWord(value, st == 0 ? pair.vs : pair.ps);
			const auto index = value.value - program.user_data_base;
			return index < user_data[st].size() ? user_data[st][index] : 0;
		};
		const auto address = [&](const ShaderRecompiler::IR::TablePlan::Operand& low,
		                         const ShaderRecompiler::IR::TablePlan::Operand& high) {
			return (uint64_t {operand(low)} | uint64_t {operand(high)} << 32u) & 0x0000ffffffffffffull;
		};
		using Plan = ShaderRecompiler::IR::TablePlan;
		for (const auto& entry: program.table_plan.indirect) {
			auto& found = table.probe;
			if (!ShaderRecompiler::IR::EnumerateAddressProbe(entry.probe, program.info.images[entry.root],
			                                                 address(entry.records_low, entry.records_high),
			                                                 address(entry.heap_low, entry.heap_high), runtime, found))
				return false;
			const auto count = static_cast<uint32_t>(found.descriptors.size());
			if (count == 0 || count > Plan::IndirectCapacity || found.keys.size() > Plan::IndirectMaxKeys ||
			    found.candidates.size() != found.keys.size())
				return false;
			std::array<uint32_t, Plan::IndirectCapacity> order {}, position {};
			for (uint32_t j = 0; j < count; ++j) order[j] = j;
			std::sort(order.begin() + 1, order.begin() + count,
			          [&](uint32_t a, uint32_t b) { return found.descriptors[a].dwords < found.descriptors[b].dwords; });
			for (uint32_t j = 0; j < count; ++j) position[order[j]] = j;
			for (uint32_t j = 0; j < count; ++j) out.push_back(found.descriptors[order[j]]);
			std::array<uint32_t, Plan::IndirectMaxKeys> by_key {};
			const auto keys = static_cast<uint32_t>(found.keys.size());
			for (uint32_t k = 0; k < keys; ++k) by_key[k] = k;
			std::sort(by_key.begin(), by_key.begin() + keys,
			          [&](uint32_t a, uint32_t b) { return found.keys[a] < found.keys[b]; });
			auto* mapping = table.blocks[st].data() + entry.mapping;
			mapping[0]    = keys;
			for (uint32_t k = 0; k < keys; ++k) {
				mapping[1 + 2 * k] = found.keys[by_key[k]];
				mapping[2 + 2 * k] = position[found.candidates[by_key[k]]];
			}
			words.push_back(count);
			for (const auto& candidate: out) words.insert(words.end(), candidate.dwords.begin(), candidate.dwords.end());
		}
		return true;
	};
#else
	const auto indirect = [&](uint32_t) { return true; };
#endif
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& program = *pair.programs[st];
		const auto& form    = pair.packed[st];
		// (Slots no stage reads keep what an earlier draw left: the block carries them unread.)
		auto& slots = table.blocks[st];
		slots.resize(program.table_plan.BlockDwords());
		if (table.addresses.size() < pair.runs[st].size()) table.addresses.resize(pair.runs[st].size());
		if (!form.valid || form.runs.size() != pair.runs[st].size() || user_data[st].size() < form.user_words ||
		    user_data[st].size() > 64) {
			if (!by_operands(st) || !indirect(st)) return false;
			continue;
		}
		// The flat form (TablePair::packed): every operand one load from one of three arrays. A level's addresses are
		// known together: their lines load at once (the guest's tables are cold).
		const std::array<const uint32_t*, 3> bases {user_data[st].data(), slots.data(), form.constants.data()};
		const auto get = [&](uint16_t operand) { return bases[operand >> 14u][operand & 0x3fffu]; };
		uint32_t   begin = 0;
		for (const auto end: form.level_ends) {
			for (auto i = begin; i < end; ++i) {
				const auto& run     = form.runs[i];
				const bool  bounded = run.records != TablePair::PackedRun::NotBounded;
				if (!locate(get(run.address[0]), get(run.address[1]), get(run.address[2]), run.immediate, run.count,
				            table.addresses[i], bounded, bounded ? get(run.records) : 0))
					return false;
			}
			for (auto i = begin; i < end; ++i)
				if (!reader.Read(table.addresses[i], form.runs[i].count, slots.data() + form.runs[i].first)) return false;
			begin = end;
		}
		const auto first = words.size();
		words.resize(first + form.image_words.size());
		auto*  out = words.data() + first;
		size_t at  = 0;
		for (size_t i = 0; i < form.image_counts.size(); ++i) {
			std::array<uint32_t, 8> image {};
			const uint32_t          count = form.image_counts[i];
			for (uint32_t w = 0; w < count; ++w) {
				image[w]    = get(form.image_words[at + w]);
				out[at + w] = image[w] & ~NativeXprLodStatsMask[w];
			}
			at += count;
			if (i < program.bindings.mip_stats_count) lod_stats(image, count);
		}
		for (; at < form.image_words.size(); ++at) out[at] = get(form.image_words[at]);
		if (!indirect(st)) return false;
	}
	return true;
}

// The image set of the words TableEvaluate made: the last one when they are the same, else the pair's set for them
// (`variant` null: a dispatch's), proven current. Null: the normal path takes the draw or dispatch.
RenderExecutor::TableImageSet* RenderExecutor::TableSet(CommandBuffer& buffer, TablePair& pair,
                                                        const TableVariant*       variant,
                                                        const NativeXprDrawState* draw_state) {
	auto&       table = OwnTable();
	const auto& words = table.words;
	const auto  frame = m_context.FrameNumber();
	auto*       set   = pair.LastSet(Role());
	if (set == nullptr || set->words != words) {
		auto* const last     = set;
		auto* const previous = pair.PreviousSet(Role());
		auto* const memo     = std::exchange(table.memo_set, nullptr); // (KYTY_TABLE_MEMO: the set the pair and key drew with)
		const bool  verify   = std::exchange(table.memo_verify, false);
		if (memo != nullptr && memo != last && !memo->refused && memo->words == words) {
			set = memo;
			// (KYTY_TABLE_MEMO_VERIFY: the set the words' lookup finds, read only; another one is taken as the full
			// path would take it.)
			if (verify) {
				const auto found = pair.sets.find(XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)));
				auto* const full = found != pair.sets.end() && found->second->words == words ? found->second.get() : nullptr;
				if (full != memo) {
					TableMemoChecked(4u, pair.vs, pair.ps, 0);
					set = full;
					if (set == nullptr || (set->refused && frame >= set->validated_frame + TableImageSet::RetryFrames))
						set = TableResolveSet(buffer, pair, variant, draw_state, words);
				}
			}
		// (The set before the last next, without the words' hash or the map's cold bucket and node.)
		} else if ((kyty_local_table_mru.load(std::memory_order_relaxed) & 4u) != 0 && previous != nullptr && previous != last &&
		    !previous->refused && previous->words == words) {
			set = previous;
		} else {
			const auto found = pair.sets.find(XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)));
			set              = found != pair.sets.end() && found->second->words == words ? found->second.get() : nullptr;
			// (KYTY_TABLE_LEARN_PORTABLE=2: a refused set whose retry found its program translating is due once that
			// translation is done.)
			const auto retry_due = [&](const TableImageSet& s) {
				if (s.retry_wait != nullptr && TableLearnPortable() == 2) return s.retry_wait->load(std::memory_order_acquire);
				return frame >= s.validated_frame + TableImageSet::RetryFrames;
			};
			if (set == nullptr || (set->refused && retry_due(*set))) {
				auto* const retried = set;
				set                 = TableResolveSet(buffer, pair, variant, draw_state, words);
				// (A retry that came back pending: the refused set is still the slot's.)
				(void)retried;
			}
		}
		// (TableResolveSet may have cleared both: a set it replaced.)
		if (last != nullptr && pair.LastSet(Role()) == last) pair.PreviousSet(Role()) = last;
		pair.LastSet(Role()) = set;
		if (set == nullptr) return Spec::Refuse("set: none resolved"), nullptr; // (a program still translating)
	}
	set->used_frame.store(frame, std::memory_order_relaxed);
	if (set->refused) return LiveCounters::Add(LiveCounters::TableRefusedSets), Spec::Refuse("table: refused set"), nullptr;
	if (!TableValidateSet(*set)) {
		Spec::Refuse("set: validation failed");
		if (Spec::Current() != nullptr) return Spec::Refuse("table: set validation"), nullptr;
		// Resolved again at the next draw that uses it.
		const auto write = CatalogWrite();
		if (table.last_set == set) table.last_set = nullptr;
		pair.ClearSets();
		++table.epoch;
		const auto found = pair.sets.find(XXH3_64bits(set->words.data(), set->words.size() * sizeof(uint32_t)));
		NativeXprRetireSet(set->pool, set->set);
		if (found != pair.sets.end() && found->second.get() == set) pair.sets.erase(found);
		return nullptr;
	}
	// Unused image sets leave once a second (the catalog's owner drops them).
	if (Role() == 0 && frame >= table.collected + 60) {
		const auto write = CatalogWrite();
		table.collected  = frame;
		for (auto* pairs: {&table.pairs, &table.dispatches})
			for (auto& [key, held]: *pairs)
				std::erase_if(held.sets, [&](auto& item) {
					if (item.second->used_frame.load(std::memory_order_relaxed) + 60 >= frame) return false;
					held.ClearSets();
					if (table.last_set == item.second.get()) table.last_set = nullptr;
					++table.epoch;
					NativeXprRetireSet(item.second->pool, item.second->set);
					return true;
				});
	}
	return set;
}

// The pair's blocks (TableEvaluate read their slots): each stage's buffers' device addresses and sizes in dwords
// after its slots, in the table upload ring. A read-only V# is resolved once (TableXpr::resolved); a written one
// is obtained as written at every use, as the normal path's NativeStorageBuffer (the buffer cache tracks the GPU's
// writes). False: a buffer the shaders cannot take.
// The V#s' words and their resolved entries (TableXpr::entries), as soon as TableEvaluate made the slots they are read
// from: the entries' lines (a 4 MiB direct-mapped table: TLB misses too) load while the image set is found (TableSet).
void RenderExecutor::TableGather(const TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data) {
	using Kind        = ShaderRecompiler::IR::TablePlan::Operand::Kind;
	auto&      table  = OwnTable();
	const auto stages = pair.programs[1] != nullptr ? 2u : 1u;
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& program = *pair.programs[st];
		const auto& plan    = program.table_plan;
		const auto& block   = table.blocks[st];
		auto&       entries = table.entries[st];
		entries.resize(plan.buffers.size());
		const auto& form = pair.packed[st];
		const bool  flat = form.valid && form.buffer_words.size() == 4 * plan.buffers.size() &&
		                  user_data[st].size() >= form.user_words && user_data[st].size() <= 64;
		const std::array<const uint32_t*, 3> bases {user_data[st].data(), block.data(), form.constants.data()};
		const bool memo = kyty_local_table_gather_memo.load(std::memory_order_relaxed) != 0; // (KYTY_TABLE_GATHER_MEMO)
		uint32_t   memo_hits = 0;
		for (size_t i = 0; i < plan.buffers.size(); ++i) {
			auto& [words4, entry] = entries[i];
			std::array<uint32_t, 4> fresh;
			if (flat) {
				for (uint32_t w = 0; w < 4; ++w) {
					const auto operand = form.buffer_words[4 * i + w];
					fresh[w]           = bases[operand >> 14u][operand & 0x3fffu];
				}
			} else {
				for (uint32_t w = 0; w < 4; ++w) {
					const auto& operand = plan.buffers[i][w];
					fresh[w] = operand.kind == Kind::Immediate ? operand.value
					           : operand.kind == Kind::Slot    ? block[operand.value]
					           : TableCodeOperand(operand)     ? TableCodeWord(operand, st == 0 ? pair.vs : pair.ps)
					           : operand.value - program.user_data_base < user_data[st].size()
					               ? user_data[st][operand.value - program.user_data_base]
					               : 0;
				}
			}
			// (The same words as gathered last at this index hash to the same entry: kept, no hash; the prefetch stays, the
			// line is what TableBlocks reads.)
			if (memo && entry != nullptr && words4 == fresh) {
				++memo_hits;
			} else {
				words4 = fresh;
				entry  = &table.resolved[XXH3_64bits(words4.data(), sizeof(words4)) & (TableXpr::ResolvedEntries - 1)];
			}
			__builtin_prefetch(entry);
		}
		if (memo_hits != 0) LiveCounters::Add(LiveCounters::TableGatherMemoHits, memo_hits);
	}
	table.gathered = &pair;
}

bool RenderExecutor::TableBlocks(TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data,
                                 std::array<vk::DeviceAddress, 2>& blocks, bool* writes) {
	using Kind         = ShaderRecompiler::IR::TablePlan::Operand::Kind;
	auto&      table        = OwnTable();
	auto&      buffer_cache = m_context.GetBufferCache();
	auto&      textures     = m_context.GetTextureCache();
	auto&      upload       = buffer_cache.GetTableUploadBuffer();
	const auto stages       = pair.programs[1] != nullptr ? 2u : 1u;
	const auto start_epoch  = textures.StartEpoch(); // (before any question below: a later image start moves it)
	const auto image_starts = [&](uint64_t address) {
		auto& known = table.no_image_start[(address * 0x9e3779b97f4a7c15ull) >> 54u];
		if (known.first == address && known.second == start_epoch) return false;
		if (textures.HasImageStartingAt(address)) return true;
		known = {address, start_epoch};
		return false;
	};
	if (table.resolved_start != start_epoch || table.resolved_mapping != table.mapping_epoch) {
		table.resolved_start   = start_epoch;
		table.resolved_mapping = table.mapping_epoch;
		++table.special_generation;
	}
	// A tracker region's epoch, read once here (the buffers mostly share one: the line is another core's).
	std::array<std::pair<uint64_t, uint64_t>, 4> region_epochs;
	region_epochs.fill({UINT64_MAX, 0});
	const auto cpu_epoch = [&](uint64_t address, uint64_t size) {
		const auto region = address / TRACKER_REGION_SIZE;
		if (region != (address + size - 1) / TRACKER_REGION_SIZE) return buffer_cache.CpuModificationEpoch(address, size);
		auto& known = region_epochs[region & 3u];
		if (known.first != region) known = {region, buffer_cache.CpuModificationEpoch(address, size)};
		return known.second;
	};
	// A range none of whose pages the guest wrote since they were uploaded (with no write protection still queued:
	// a page marked clean can change unseen until it lands).
	const auto range_clean = [&](uint64_t address, uint64_t size) {
		for (auto region = address / TRACKER_REGION_SIZE; region <= (address + size - 1) / TRACKER_REGION_SIZE; ++region)
			if (buffer_cache.DeferredProtectionPending(region * TRACKER_REGION_SIZE)) return false;
		return !buffer_cache.IsRegionCpuModified(address, size);
	};
	// A null buffer's device address: valid memory (the shaders' loads of a buffer are not branched around its bounds:
	// out of bounds they load its first dword).
	const vk::DeviceAddress null_device = upload.BufferDeviceAddress();
	// A V#'s buffer word and stride. (The word is the V#'s fourth dword, its format and dst_sel: the programs of the
	// buffers a pair saw change format decode it at run time, IR::TablePlan::runtime_formats.)
	const auto word_of = [](const std::array<uint32_t, 4>& words4) -> std::pair<uint32_t, uint32_t> {
		ShaderBufferResource descriptor;
		std::copy_n(words4.begin(), 4, descriptor.fields);
		if (descriptor.Type() != 0) return {0, 0};
		return {words4[3], descriptor.Stride()};
	};
	// The bytes a V# names (0: none).
	const auto requested_of = [](const std::array<uint32_t, 4>& words4) -> uint64_t {
		ShaderBufferResource descriptor;
		std::copy_n(words4.begin(), 4, descriptor.fields);
		if (descriptor.Type() != 0 || descriptor.Base48() == 0) return 0;
		return descriptor.Stride() != 0 ? uint64_t {descriptor.Stride()} * descriptor.NumRecords()
		                                : uint64_t {descriptor.NumRecords()};
	};
	// As RenderExecutor::FindBuffers: a V# that is no buffer, or of nothing mapped, is a null buffer (size 0).
	const auto range_of = [](const std::array<uint32_t, 4>& words4) -> std::pair<uint64_t, uint64_t> {
		ShaderBufferResource descriptor;
		std::copy_n(words4.begin(), 4, descriptor.fields);
		const auto requested = descriptor.Stride() != 0 ? uint64_t {descriptor.Stride()} * descriptor.NumRecords()
		                                                : uint64_t {descriptor.NumRecords()};
		const auto address   = descriptor.Base48();
		return {address, descriptor.Type() != 0 || address == 0 || requested == 0
		                     ? 0
		                     : Libs::LibKernel::Memory::ClampRangeSize(address, requested)};
	};
	// A V# the program was specialized for (as SpecializeDirectResources derives it from the V#): another one takes the
	// normal path, which chooses the program for it.
	const auto specialized = [&](uint32_t st, size_t i, const std::array<uint32_t, 4>& words4) {
		const auto& resources = pair.programs[st]->info.buffers;
		const auto& specified = pair.tables[st]->specialization.buffers;
		return i < specified.size() &&
		       TableBufferSpecialized(resources[i], specified[i], words4,
		                              i < 32 && ((pair.programs[st]->table_plan.runtime_formats >> i) & 1u) != 0);
	};
	if (table.gathered != &pair) TableGather(pair, user_data);
	table.gathered = nullptr; // (taken: a later draw or dispatch gathers its own)
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& program = *pair.programs[st];
		const auto& plan    = program.table_plan;
		auto&       block   = table.blocks[st];
		auto&       entries = table.entries[st];
		// The buffers it writes first: a range it also reads through another V# is GPU-written by then, as the
		// normal path's second lookup (RebindBuffers) sees it (not a copy of the guest's bytes in the ring).
		for (size_t n = 0; n < 2 * plan.buffers.size(); ++n) {
			const auto  i         = n < plan.buffers.size() ? n : n - plan.buffers.size();
			const auto& words4    = entries[i].first;
			const auto& resource  = program.info.buffers[i];
			const bool  formatted = resource.formatted;
			if ((resource.written || resource.atomic) != (n < plan.buffers.size())) continue;
			if (!specialized(st, i, words4)) {
				// A buffer whose V#s differ in format: the pair's programs are compiled again with that buffer's
				// format decoded at run time (TableReset, TableStore).
				if (Role() == 0 && i < 32 && ((plan.runtime_formats >> i) & 1u) != 0 && ((pair.portable[st] >> i) & 1u) == 0) {
					const auto write = CatalogWrite();
					pair.portable[st] |= 1u << i;
					pair.respecialize = true;
				}
				return Spec::Refuse("blocks: another V# specialization"), false;
			}
			if (resource.written || resource.atomic) {
				// The shader writes it: marked GPU-written now, whatever an earlier use proved. (A write hazard as
				// HasShaderBufferWrites sees one: a V# of a nonempty range.)
				if (writes != nullptr && resource.written && (uint64_t {words4[0]} | (words4[1] & 0xffffu)) != 0 &&
				    words4[2] != 0)
					*writes = true;
				auto& proof = table.written[XXH3_64bits(words4.data(), sizeof(words4)) & (TableXpr::WrittenEntries - 1)];
				if (proof.special != table.special_generation || proof.words != words4) {
					const auto [clamped_address, clamped_size] = range_of(words4);
					proof = {.words = words4, .special = table.special_generation, .address = clamped_address, .size = clamped_size};
				}
				const auto address = proof.address, size = proof.size;
				vk::DeviceAddress device = null_device;
				if (size != 0) {
					if (address % 4 != 0) return Spec::Refuse("blocks: written unaligned"), false;
					if (formatted && !proof.image_checked) {
						if (image_starts(address)) return Spec::Refuse("blocks: written image start"), false;
						proof.image_checked = true;
					}
					const auto [owner, offset] = buffer_cache.ObtainBuffer(address, size, true, formatted);
					region_epochs.fill({UINT64_MAX, 0}); // (its uploads may have moved them)
					if (owner == nullptr || buffer_cache.GetBufferIfLive(buffer_cache.FindBuffer(address, size)) != owner)
						return Spec::Refuse("blocks: written owner"), false;
					if (formatted) textures.InvalidateMemoryFromGPU(address, size);
					device = owner->BufferDeviceAddress() + offset;
				}
				const auto [word, stride]      = word_of(words4);
				block[plan.BufferDword(i)]     = static_cast<uint32_t>(device);
				block[plan.BufferDword(i) + 1] = static_cast<uint32_t>(device >> 32u);
				block[plan.BufferDword(i) + 2] = static_cast<uint32_t>(std::min<uint64_t>(size / 4, UINT32_MAX));
				block[plan.BufferDword(i) + 3] = word;
				block[plan.BufferDword(i) + 4] = stride;
				continue;
			}
			auto& held    = *entries[i].second;
			bool  current = held.words == words4 && (held.special == 0 || held.special == table.special_generation);
			if (current && (held.registration & TableXpr::RingCopyMark) != 0) {
				// Its ring copy (RingCopy's terms: the same observation epoch, the regions' epoch unchanged, no page
				// GPU-written).
				if (Role() == 0 && Spec::Current() == nullptr &&
				    held.registration == (TableXpr::RingCopyMark | DrawWindow()) &&
				    cpu_epoch(held.address, held.size) == held.cpu_epoch &&
				    !buffer_cache.IsRegionGpuModified(held.address, held.size)) {
					LiveCounters::Add(LiveCounters::TableRingShared);
					LiveCounters::Add(LiveCounters::TableRingSharedBytes, held.size);
					const auto [word, stride]      = word_of(words4);
					block[plan.BufferDword(i)]     = static_cast<uint32_t>(held.device);
					block[plan.BufferDword(i) + 1] = static_cast<uint32_t>(held.device >> 32u);
					block[plan.BufferDword(i) + 2] = held.dwords;
					block[plan.BufferDword(i) + 3] = word;
					block[plan.BufferDword(i) + 4] = stride;
					continue;
				}
				current = false;
			}
			// (A speculation's guest memory writes wait for its commit: such a range is resolved, ObtainBuffer decides.)
			if (const auto* spec = Spec::Current();
			    current && held.size != 0 && spec != nullptr && spec->JournalIntersects(held.address, held.size))
				current = false;
			if (const auto registration = buffer_cache.RegistrationEpoch();
			    current && held.size != 0 && held.registration != registration) {
				// Buffers were registered or unregistered since: it holds while none covered its tracker regions. (The
				// epoch read before the regions': a speculative translation's thread runs beside the render thread's
				// registrations. The render thread's own entries cannot meet one midway: it registers itself. A
				// worker's entries outlive its translation, in its executor's table, so an epoch read after a
				// registration began proved them for later translations too.)
				for (auto region = held.address / TRACKER_REGION_SIZE, last = (held.address + held.size - 1) / TRACKER_REGION_SIZE;
				     current && region <= last; ++region)
					current = buffer_cache.RegionRegistrationEpoch(region * TRACKER_REGION_SIZE) <= held.registration;
				if (current) held.registration = registration;
			}
			if (current && held.size != 0) {
				// Another page of its tracker regions was written: the range itself may still be clean.
				if (const auto epoch = cpu_epoch(held.address, held.size); epoch == 0 || epoch != held.cpu_epoch) {
					LiveCensus::Scope reprove(LiveCensus::DrawPhase, 0x7ab1e, 6);
					current = range_clean(held.address, held.size);
					if (current) held.cpu_epoch = epoch;
				}
			}
			if (!current) {
				LiveCensus::Scope resolve(LiveCensus::DrawPhase, 0x7ab1e, 5);
				// (Kept only once resolved: a refusal leaves nothing a later use could take.)
				held = {};
				TableXpr::ResolvedBuffer fresh {.words = words4, .device = null_device};
				const auto [address, size] = range_of(words4);
				if (formatted || size < requested_of(words4)) fresh.special = table.special_generation;
				if (size != 0) {
					// The shader reads whole dwords from the device address; a buffer an image starts at is made from
					// the image by ObtainBuffer only (SynchronizeBufferFromImage).
					if (address % 4 != 0 || (formatted && image_starts(address)))
						return Spec::Refuse(address % 4 != 0 ? "blocks: read unaligned" : "blocks: read image start"), false;
					// (A speculation's: the guest's writes, not those its commit makes current; with none of its own
					// guest memory writes pending over the range, read as no GPU work wrote it.)
					auto* const spec        = Spec::Current();
					const bool  cpu_written = buffer_cache.IsRegionCpuDirty(address, size) &&
					                         !buffer_cache.IsRegionGpuModified(address, size);
					if (cpu_written && size <= BufferCache::CACHING_PAGESIZE &&
					    (spec == nullptr || !spec->JournalIntersects(address, size))) {
						// As ObtainBuffer: a small range the guest wrote is read from a copy in the ring (nothing held: the
						// main executor's later uses share it while it holds, TableXpr::RingCopy), not uploaded into its
						// buffer.
						auto* const shared = spec == nullptr && Role() == 0
						                         ? &table.ring_copies[((address ^ (size << 40u)) * 0x9e3779b97f4a7c15ull) >> 52u]
						                         : nullptr;
						const auto observation = DrawWindow();
						const auto tick        = m_context.GetCommandScheduler().CurrentTick();
						const auto cpu         = buffer_cache.CpuModificationEpoch(address, size); // (before the copy)
						vk::DeviceAddress device = 0;
						if (shared != nullptr && cpu != 0 && shared->address == address && shared->size == size &&
						    shared->observation == observation && shared->tick == tick && shared->cpu_epoch == cpu) {
							device = shared->device;
							LiveCounters::Add(LiveCounters::TableRingShared);
							LiveCounters::Add(LiveCounters::TableRingSharedBytes, size);
						} else if (auto [mapped, offset] = upload.Map(size, 16, false);
						           mapped != nullptr && Libs::LibKernel::Memory::TryReadBacking(address, mapped, size)) {
							upload.Commit();
							if (spec != nullptr) spec->NoteCleanRead(address, size);
							device = upload.BufferDeviceAddress() + offset;
							LiveCounters::Add(LiveCounters::TableRingCopies);
							LiveCounters::Add(LiveCounters::TableRingBytes, size);
							if (shared != nullptr && cpu != 0)
								*shared = {.address     = address,
								           .size        = size,
								           .observation = observation,
								           .tick        = tick,
								           .cpu_epoch   = cpu,
								           .device      = device};
						}
						// (The V#'s later uses take it from its held entry.)
						if (device != 0 && shared != nullptr && cpu != 0)
							held = {.words        = words4,
							        .dwords       = static_cast<uint32_t>(size / 4),
							        .special      = fresh.special,
							        .device       = device,
							        .address      = address,
							        .size         = size,
							        .cpu_epoch    = cpu,
							        .registration = TableXpr::RingCopyMark | observation};
						if (device != 0) {
							const auto [word, stride]      = word_of(words4);
							block[plan.BufferDword(i)]     = static_cast<uint32_t>(device);
							block[plan.BufferDword(i) + 1] = static_cast<uint32_t>(device >> 32u);
							block[plan.BufferDword(i) + 2] = static_cast<uint32_t>(size / 4);
							block[plan.BufferDword(i) + 3] = word;
							block[plan.BufferDword(i) + 4] = stride;
							continue;
						}
					}
					if (cpu_written) buffer_cache.EnsureBufferContents(address, size);
					const auto registration_before = buffer_cache.RegistrationEpoch(); // (a speculation's: before its lookup)
					const auto [owner, offset] = buffer_cache.ObtainBuffer(address, size, false, formatted);
					region_epochs.fill({UINT64_MAX, 0});
					// (The epoch before the dirty check: a write after it moves the epoch.)
					fresh.cpu_epoch = buffer_cache.CpuModificationEpoch(address, size);
					if (owner == nullptr || buffer_cache.GetBufferIfLive(buffer_cache.FindBuffer(address, size)) != owner ||
					    buffer_cache.IsRegionCpuModified(address, size))
						return Spec::Refuse(owner == nullptr ? "blocks: read no owner"
						                    : buffer_cache.IsRegionCpuModified(address, size) ? "blocks: read still CPU-modified"
						                                                                       : "blocks: read owner not live"),
						       false;
					if (Spec::Current() != nullptr && buffer_cache.IsRegionCpuDirty(address, size)) fresh.cpu_epoch = 0;
					fresh.device       = owner->BufferDeviceAddress() + offset;
					fresh.dwords       = static_cast<uint32_t>(std::min<uint64_t>(size / 4, UINT32_MAX));
					fresh.address      = address;
					fresh.size         = size;
					// (The owner's: after ObtainBuffer's own registrations. A speculation's: before its lookup, which the
					// render thread's registrations may pass.)
					fresh.registration = Spec::Current() != nullptr ? registration_before : buffer_cache.RegistrationEpoch();
				}
				held = fresh;
			}
			// (A speculation's: the buffer of the range its device address is in.)
			if (auto* spec = Spec::Current(); spec != nullptr && held.size != 0) spec->NoteBufferRange(held.address, held.size);
			const auto [word, stride]      = word_of(words4);
			block[plan.BufferDword(i)]     = static_cast<uint32_t>(held.device);
			block[plan.BufferDword(i) + 1] = static_cast<uint32_t>(held.device >> 32u);
			block[plan.BufferDword(i) + 2] = held.dwords;
			block[plan.BufferDword(i) + 3] = word;
			block[plan.BufferDword(i) + 4] = stride;
		}
		if (plan.BlockDwords() != 0) { // (a stage with neither slots nor buffers reads no block)
			// (A speculation's thread never waits for its ring: it holds the catalog shared; the hole runs the draw.)
			const auto copied = Spec::Current() == nullptr
			                        ? std::optional<uint64_t> {upload.Copy(block.data(), block.size() * sizeof(uint32_t), 16)}
			                        : upload.TryCopy(block.data(), block.size() * sizeof(uint32_t), 16);
			if (!copied) return Spec::Refuse("table: upload ring busy"), false;
			blocks[st] = upload.BufferDeviceAddress() + *copied;
		}
	}
	return true;
}

// The page table ranges the pair's flat and global loads read made current, as the normal path's PrepareBdaBindings
// does at every use (the read plans' words from the user data and the blocks' slots); an unbounded plan, all of them.
// (Without it, particle simulations at Stonefang read stale pages: readbacks the game crashed on, and indirect
// arguments that lost the device.)
void RenderExecutor::TablePrepareBda(const TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data) {
	auto&                                    table = OwnTable();
	std::array<const ShaderStageRuntime*, 2> stages {};
	uint32_t                                 count = 0;
	for (uint32_t st = 0; st < 2; ++st) {
		const auto* program = pair.programs[st];
		if (program == nullptr || !program->info.uses_dma) continue;
		auto& snapshot = table.snapshots[st];
		snapshot.user_data.assign(user_data[st].begin(), user_data[st].end());
		snapshot.flattened_srt.assign(table.blocks[st].begin(),
		                              table.blocks[st].begin() + static_cast<std::ptrdiff_t>(program->table_plan.slots.size()));
		table.runtimes[st] = {.module = pair.tables[st], .resources = &snapshot};
		m_context.CacheDmaBases(table.runtimes[st]);
		stages[count++] = &table.runtimes[st];
	}
	if (count == 0) return;
	const bool reach_synchronized = m_context.SynchronizeDmaFootprint(std::span {stages.data(), count});
	const bool writes = (pair.programs[0] != nullptr && pair.programs[0]->has_address_writes) ||
	                    (pair.programs[1] != nullptr && pair.programs[1]->has_address_writes);
	m_context.PrepareBda(writes, !reach_synchronized);
}

vk::AccessFlags2 RenderExecutor::TableTargetRead(const TableImageSet& set, ImageId id) {
	vk::AccessFlags2 read {};
	for (const auto& dep: set.images)
		if (dep.target && dep.id == id) read |= dep.target_read;
	return read;
}

// KYTY_TABLE_SERIALS=2: a check the serial sums would have skipped that changed something (the first 32 printed).
static void TableSerialsUnsafe(const char* what) {
	static std::atomic<uint32_t> count {0};
	LiveCounters::Add(LiveCounters::SerialsUnsafe);
	if (const auto n = count.fetch_add(1, std::memory_order_relaxed); n < 32)
		std::printf("TABLE-SERIALS UNSAFE: %s (%u)\n", what, n + 1);
}

// The image serial sums (ImageSerials) of a draw state's targets and of a set's images, with the unattributed serial.
template<typename State> // (RenderExecutor::NativeXprDrawState)
static uint64_t TableTargetSerials(const State& draw_state) {
	uint64_t sum = ImageSerials::Unattributed();
	for (const auto id: draw_state.target_ids) sum += ImageSerials::Read(id.index);
	return sum;
}
template<typename Set> // (RenderExecutor::TableImageSet)
static uint64_t TableSetSerials(const Set& set) {
	uint64_t sum = ImageSerials::Unattributed();
	for (const auto id: set.image_ids) sum += ImageSerials::Read(id.index);
	return sum;
}
// A set transited under another draw state is in place under this one when it holds no target of either: TableTransit
// asks the state only which of the set's images it targets.
template<typename Set, typename State>
static bool TableSetApart(const Set& set, const State& draw_state) {
	if (set.holds_target) return false;
	for (const auto id: set.image_ids)
		if (std::ranges::find(draw_state.target_ids, id) != draw_state.target_ids.end()) return false;
	return true;
}

// A set's images in their layouts (other work may have moved them since the set was last bound), unless the draw's
// targets are not those the set was made for (an image held as a texture became a target, or the reverse) or an image
// needs what the normal path's acquisition does first (an upload, a pending fast clear, a storage image the GPU does
// not own). (His version applies a DCC image's pending fast clears here with FindTexture; the normal path takes that
// draw here.)
bool RenderExecutor::TableTransit(const TableImageSet& set, CommandRecorder vk_buffer,
                                  const NativeXprDrawState* draw_state) {
	auto& textures = m_context.GetTextureCache();
	for (const auto& dep: set.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial) return false;
		// A target of the draw: NativeXprEmitTargets put it in its attachment layout, which the set must have been
		// made for; an image the set holds as a texture must not be one.
		const auto target = draw_state == nullptr
		                        ? decltype(draw_state->targets.end()) {}
		                        : std::ranges::find(draw_state->targets, dep.id, &NativeXprDrawState::Target::id);
		const bool is_target = draw_state != nullptr && target != draw_state->targets.end();
		if (is_target != dep.target ||
		    (is_target && (target->layout != dep.layout || (target->access | dep.target_read) != dep.access)))
			return false;
		if (is_target) continue;
		// An image the shader writes is GPU-modified, CPU-clean and current with buffer writes (as the normal path's
		// FindTexture leaves a storage image: it acquires it again otherwise). One written since the set was proven this
		// frame (by the CPU, a GPU buffer write, or into its stencil plane) is uploaded by the normal path's FindTexture.
		if ((dep.storage && !image->SafeToDownload()) || TableRefreshPending(*image, dep.desc)) return false;
		if (dep.desc.info.metadata.kind == ImageMetadataKind::Dcc) {
			if (image->info.IsVolume()) return false;
			// His surface metadata's clear mask (IsMetaCleared): this texture cache keeps a DCC fast clear in the
			// metadata's bytes instead (ColorClearSettled), and the normal path's lookup applies it.
			if (!textures.ColorClearSettled(dep.desc, dep.clear_memo)) return false;
		}
	}
	// As CommitBindings': the transitions all precede the one draw or dispatch.
	for (const auto& dep: set.images)
		if (!dep.target)
			textures.m_slot_images[dep.id].Transit(dep.layout, dep.access, dep.whole ? std::nullopt : std::optional {dep.range},
			                                       vk_buffer);
		else if (dep.target_read) // (as CommitBindings transits a target the shaders sample: the whole image)
			textures.m_slot_images[dep.id].Transit(dep.layout, dep.access, std::nullopt, vk_buffer);
	return true;
}

#if KYTY_CX_SCOUT
// KYTY_SCOUT (scout.h): the draw pair's view for the scout, when its stages take TableEvaluate's and TableGather's flat
// paths with no indirect image; none otherwise.
template <typename Pair>
static void TableScoutPublish(Pair& pair) {
	if (!Scout::Running() || pair.programs[0] == nullptr) return;
	auto view    = std::make_unique<Scout::View>();
	view->key    = {pair.vs, pair.ps, static_cast<uint32_t>(pair.start_instance)};
	view->stages = pair.programs[1] != nullptr ? 2u : 1u;
	for (uint32_t st = 0; st < view->stages; ++st) {
		const auto& program = *pair.programs[st];
		const auto& plan    = program.table_plan;
		const auto& form    = pair.packed[st];
		if (!form.valid || form.runs.size() != pair.runs[st].size() || form.user_words > 32 ||
#if KYTY_CX_INDIRECT
		    !plan.indirect.empty() ||
#endif
		    form.buffer_words.size() != 4 * plan.buffers.size())
			return;
		auto& stage = view->stage[st];
		for (const auto& run: form.runs) {
			stage.runs.push_back({run.address, run.records, run.immediate, run.first, run.count});
			stage.slot_words += run.count;
		}
		if (stage.slot_words > 0xffff) return;
		// Block order: the runs by first slot, side by side ones one segment.
		std::vector<uint16_t> order(stage.runs.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint16_t>(i);
		std::ranges::sort(order, {}, [&](uint16_t i) { return stage.runs[i].first; });
		uint16_t at = 0;
		for (const auto i: order) {
			auto& run = stage.runs[i];
			run.at    = at;
			at        = static_cast<uint16_t>(at + run.count);
			if (!stage.segments.empty() && stage.segments.back().first + stage.segments.back().count == run.first)
				stage.segments.back().count = static_cast<uint16_t>(stage.segments.back().count + run.count);
			else
				stage.segments.push_back({run.first, run.count});
		}
		stage.level_ends.assign(form.level_ends.begin(), form.level_ends.end());
		stage.image_words.assign(form.image_words.begin(), form.image_words.end());
		stage.buffer_words.assign(form.buffer_words.begin(), form.buffer_words.end());
		stage.image_counts.assign(form.image_counts.begin(), form.image_counts.end());
		stage.constants.assign(form.constants.begin(), form.constants.end());
		stage.user_words      = form.user_words;
		stage.block_dwords    = static_cast<uint32_t>(plan.BlockDwords());
		stage.lod_stats_count = static_cast<uint32_t>(program.bindings.mip_stats_count);
		stage.buffers         = static_cast<uint32_t>(plan.buffers.size());
	}
	view->version = pair.scout_version = Scout::NextVersion();
	Scout::Publish(view.release(), TableScoutHash(pair));
}

// The scout's ticket for the draw TableDraw makes (Scout::Current), when it holds: made for this packet from this
// pair's view, with the user data the render thread has, under the mapping epoch of now, and no page of its tables with
// read watchers now (GPU work recorded after the scout read them writes it). Null otherwise, counted by reason.
template <typename Pair>
static const Scout::Ticket* TableScoutTicket(const Pair& pair, std::array<std::span<const uint32_t>, 2> user_data,
                                             uint32_t stages, RenderContext& resources, const Scout::Ticket* ahead) {
	LiveCounters::Add(LiveCounters::ScoutChecks);
	if (pair.scout_version == 0) return LiveCounters::Add(LiveCounters::ScoutLostNoView), nullptr;
	// (KYTY_SCOUT=3: the operation's own ticket, made by the resolve thread for this draw's registers: no scout, no
	// identity to check.)
	const bool  pipelined = Scout::Mode() == 3;
	const auto* ticket    = pipelined ? ahead : Scout::Current();
	if (ticket == nullptr) {
		LiveCounters::Add(LiveCounters::ScoutLostMissing);
		static_assert(LiveCounters::ScoutMissFenced - LiveCounters::ScoutMissUnwalked == static_cast<uint32_t>(Scout::Reason::Fenced)); // (the miss counters follow Reason)
		if (!pipelined)
			LiveCounters::Add(static_cast<LiveCounters::Id>(LiveCounters::ScoutMissUnwalked + static_cast<uint32_t>(Scout::CurrentReason())));
		return nullptr;
	}
	if (!pipelined && ticket->packet != Scout::CurrentPacket()) return LiveCounters::Add(LiveCounters::ScoutLostIdentity), nullptr;
	if (ticket->version != pair.scout_version || ticket->stages != stages)
		return LiveCounters::Add(LiveCounters::ScoutLostPair), nullptr;
	for (uint32_t st = 0; st < stages; ++st)
		if (ticket->user_words[st] > user_data[st].size() ||
		    std::memcmp(ticket->user_data[st].data(), user_data[st].data(), ticket->user_words[st] * sizeof(uint32_t)) != 0)
			return LiveCounters::Add(LiveCounters::ScoutLostRegisters), nullptr;
	if (ticket->mapping_epoch != resources.MappingEpoch()) return LiveCounters::Add(LiveCounters::ScoutLostEpoch), nullptr;
	if (bool write_back = false; Scout::WrittenSince(*ticket, write_back)) {
		if (write_back) LiveCounters::Add(LiveCounters::ScoutLostWriteBack);
		return LiveCounters::Add(LiveCounters::ScoutLostWritten), nullptr;
	}
	// Each page once (in order), its granule's bit once a granule.
	const auto* pages    = Scout::Pages(*ticket);
	const auto* granules = resources.ReadGranules();
	uint64_t    clear    = UINT64_MAX; // a granule whose bit is clear
	for (uint32_t i = 0; i < ticket->pages; ++i) {
		if ((pages[2 * i + 1] & Scout::Ticket::PageBacking) != 0) continue; // (its ranges are proven below)
		const auto page    = Scout::PageAt(pages, i);
		const auto granule = page >> (PageManager::READ_GRANULE_BITS - 12u);
		if (granule == clear) continue;
		if ((page << 12u) < TRACKER_ADDRESS_SIZE &&
		    ((granules[granule / 64u].load(std::memory_order_acquire) >> (granule % 64u)) & 1u) == 0) {
			clear = granule;
			continue;
		}
		if (resources.HasReadWatchers(page << 12u, 4096)) return LiveCounters::Add(LiveCounters::ScoutLostWatched), nullptr;
	}
	// Each range the scout read from the backing, as this thread's own read takes it (clean bytes
	// from the backing; otherwise the GPU's, read back) and the same as the ticket's.
	const auto* ranges = pages + 2u * ticket->pages;
	for (uint32_t i = 0; i < ticket->backing; ++i) {
		const auto               address = ranges[3 * i] | uint64_t {ranges[3 * i + 1]} << 32u;
		const auto*              held    = ticket->data + (ranges[3 * i + 2] & 0xffffu);
		const uint32_t           count   = ranges[3 * i + 2] >> 16u;
		std::array<uint32_t, 64> fresh;
		for (uint32_t done = 0; done < count; done += static_cast<uint32_t>(fresh.size())) {
			const auto chunk = std::min<uint32_t>(count - done, static_cast<uint32_t>(fresh.size()));
			if (!Libs::LibKernel::Memory::TryReadGpuCleanBackingToHost(address + 4ull * done, fresh.data(), chunk * 4ull))
				return LiveCounters::Add(LiveCounters::ScoutLostDirty), nullptr;
			if (std::memcmp(fresh.data(), held + done, chunk * 4ull) != 0)
				return LiveCounters::Add(LiveCounters::ScoutLostChanged), nullptr;
		}
	}
	LiveCounters::Add(LiveCounters::ScoutUsed);
	if (ticket->backing != 0) LiveCounters::Add(LiveCounters::ScoutUsedBacking);
	return ticket;
}

// Words copied without a library call: a ticket's segments are a few words each, and a memcpy call a run cost more
// than its copy.
static inline void TableCopyWords(uint32_t* to, const uint32_t* from, uint32_t count) {
	for (; count >= 8; count -= 8, to += 8, from += 8) __builtin_memcpy_inline(to, from, 32);
	if ((count & 4u) != 0) {
		__builtin_memcpy_inline(to, from, 16);
		to += 4;
		from += 4;
	}
	if ((count & 2u) != 0) {
		__builtin_memcpy_inline(to, from, 8);
		to += 2;
		from += 2;
	}
	if ((count & 1u) != 0) *to = *from;
}

// A ticket's slot words of one stage (`in`) into its block `slots`, by segment (`segments`, `count` of them).
static void TableScoutSlots(uint32_t* slots, const uint32_t* in, const uint32_t* segments, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t words = segments[i] >> 16u;
		TableCopyWords(slots + (segments[i] & 0xffffu), in, words);
		in += words;
	}
}

// A ticket's evaluation where TableEvaluate and TableGather leave theirs: each stage's runs' slots in its block (the
// other slots as they were), its T#/S# and LOD statistics words, and its V#s with their entries.
template <typename Table, typename Pair>
static void TableScoutApply(Table& table, const Pair& pair, const Scout::Ticket& ticket, uint32_t stages) {
	const uint32_t* in       = ticket.data;
	const uint32_t* segments = ticket.data + ticket.slot_words[0] + ticket.slot_words[1];
	for (uint32_t st = 0; st < stages; ++st) {
		auto& slots = table.blocks[st];
		slots.resize(pair.programs[st]->table_plan.BlockDwords());
		TableScoutSlots(slots.data(), in, segments, ticket.segments[st]);
		in += ticket.slot_words[st];
		segments += ticket.segments[st];
		table.indirect[st].clear();
	}
	in = segments;
	table.words.assign(in, in + ticket.image_words);
	in += ticket.image_words;
	table.lod_stats.assign(in, in + ticket.lod_stats);
	in += ticket.lod_stats;
	for (uint32_t st = 0; st < stages; ++st) {
		auto& entries = table.entries[st];
		entries.resize(ticket.buffers[st]);
		for (auto& [words4, entry]: entries) {
			std::copy_n(in, 4, words4.begin());
			entry = &table.resolved[in[4]];
			__builtin_prefetch(entry);
			in += 5;
		}
	}
	table.gathered = &pair;
}

// KYTY_SCOUT=2: the ticket against the render thread's own evaluation (`evaluated`: TableEvaluate's result, then
// TableGather's), field by field; a difference is counted and the first ones reported, the first slot that differs with
// both values.
template <typename Table, typename Pair>
static void TableScoutVerify(const Table& table, const Pair& pair, const Scout::Ticket& ticket, uint32_t stages,
                             bool evaluated) {
	// slots, T#/S# words, LOD statistics words, V#s and entries, and the apply (its slots right, the block it copies
	// them into wrong)
	std::array<bool, 5> differ {};
	char                detail[160] = "";
	if (evaluated) {
		const uint32_t* in       = ticket.data;
		const uint32_t* segments = ticket.data + ticket.slot_words[0] + ticket.slot_words[1];
		thread_local std::vector<uint32_t> scratch;
		for (uint32_t st = 0; st < stages; ++st) {
			const auto& runs   = pair.packed[st].runs;
			const bool  before = differ[0];
			for (size_t r = 0; r < runs.size(); ++r) {
				const auto& run  = runs[r];
				const auto* mine = table.blocks[st].data() + run.first;
				// The run's words, in block order: its segment's, then its own.
				uint32_t at = 0;
				for (uint32_t k = 0; k < ticket.segments[st]; ++k) {
					const uint32_t first = segments[k] & 0xffffu, count = segments[k] >> 16u;
					if (run.first >= first && run.first < first + count) {
						at += run.first - first;
						break;
					}
					at += count;
				}
				for (uint32_t w = 0; w < run.count && !differ[0]; ++w) {
					if (mine[w] == in[at + w]) continue;
					differ[0] = true;
					std::snprintf(detail, sizeof(detail), " (stage %u run %zu/%zu slot %u: ticket 0x%08x, render 0x%08x)", st, r,
					              runs.size(), run.first + w, in[at + w], mine[w]);
				}
			}
			// The ticket applied as TableScoutApply does, into a copy of the render thread's block.
			scratch.assign(table.blocks[st].begin(), table.blocks[st].end());
			TableScoutSlots(scratch.data(), in, segments, ticket.segments[st]);
			if (differ[0] == before && scratch != table.blocks[st]) {
				differ[4] = true;
				std::snprintf(detail, sizeof(detail), " (stage %u: the apply's block differs)", st);
			}
			LiveCounters::Add(LiveCounters::ScoutApplyRuns, runs.size());
			LiveCounters::Add(LiveCounters::ScoutApplySegments, ticket.segments[st]);
			segments += ticket.segments[st];
			in += ticket.slot_words[st];
		}
		in = segments;
		differ[1] = table.words.size() != ticket.image_words || !std::equal(table.words.begin(), table.words.end(), in);
		in += ticket.image_words;
		differ[2] = table.lod_stats.size() != ticket.lod_stats || !std::equal(table.lod_stats.begin(), table.lod_stats.end(), in);
		in += ticket.lod_stats;
		for (uint32_t st = 0; st < stages; ++st) {
			if (table.entries[st].size() != ticket.buffers[st]) {
				differ[3] = true;
				break;
			}
			for (const auto& [words4, entry]: table.entries[st]) {
				differ[3] = differ[3] || !std::equal(words4.begin(), words4.end(), in) || entry != &table.resolved[in[4]];
				in += 5;
			}
		}
		if (differ == std::array<bool, 5> {}) return;
	}
	LiveCounters::Add(LiveCounters::ScoutMismatches);
	if (!evaluated) LiveCounters::Add(LiveCounters::ScoutMismatchRefused);
	if (differ[0]) LiveCounters::Add(LiveCounters::ScoutMismatchSlots);
	if (differ[1]) LiveCounters::Add(LiveCounters::ScoutMismatchWords);
	if (differ[2]) LiveCounters::Add(LiveCounters::ScoutMismatchLod);
	if (differ[3]) LiveCounters::Add(LiveCounters::ScoutMismatchEntries);
	if (differ[4]) LiveCounters::Add(LiveCounters::ScoutMismatchApply);
	static std::atomic<uint32_t> reports {0};
	if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
		std::printf("Scout: mismatch VS 0x%llx PS 0x%llx submission %llu draw %u:%s%s%s%s%s%s%s\n",
		            static_cast<unsigned long long>(pair.vs), static_cast<unsigned long long>(pair.ps),
		            static_cast<unsigned long long>(ticket.sequence), ticket.ordinal, evaluated ? "" : " refused",
		            differ[0] ? " slots" : "", differ[1] ? " words" : "", differ[2] ? " lod" : "", differ[3] ? " entries" : "",
		            differ[4] ? " apply" : "", detail);
}

#endif // KYTY_CX_SCOUT

RenderExecutor::TableResult RenderExecutor::TableDraw(CommandBuffer& buffer, TablePair& pair, TableVariant& variant,
                                                      const NativeXprDrawState& draw_state,
                                                      std::span<const uint64_t> commands, uint64_t index_base,
                                                      uint64_t index_bytes, vk::IndexType index_type,
                                                      const NativeXprDirectDraw* direct) {
	auto&       table  = OwnTable();
	// (A speculative translation's state, read once behind a plain global: KYTY_SPECULATE unset, no thread-local
	// reads in the draw.)
	Spec::State* const spec_state =
	    ::kyty_local_speculate_mode.load(std::memory_order_relaxed) != 0 ? Spec::Current() : nullptr;
	auto&       cache  = *m_native_xpr;
	const auto& sh     = buffer.GetShaders();
	const auto  stages = pair.programs[1] != nullptr ? 2u : 1u;
	const std::array<std::span<const uint32_t>, 2> user_data {
	    std::span<const uint32_t>(sh.GetVs().gs_user_sgpr.value,
	                              std::min<uint32_t>(sh.GetVs().gs_regs.rsrc2.user_sgpr, HW::UserSgprInfo::SGPRS_MAX)),
	    std::span<const uint32_t>(sh.GetPs().ps_user_sgpr.value,
	                              std::min<uint32_t>(sh.GetPs().ps_regs.rsrc2.user_sgpr, HW::UserSgprInfo::SGPRS_MAX))};
	// (The index buffer's held entry, a cold line of a 320 KiB table, loads while the tables are evaluated.)
	if (index_base != 0) {
		const auto* held = &table.indices[(index_base * 0x9e3779b97f4a7c15ull) >> 51u];
		__builtin_prefetch(held);
		__builtin_prefetch(reinterpret_cast<const char*>(held + 1) - 1);
	}
	std::optional<LiveCensus::Scope> phase;
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1e, 1);
	// KYTY_SCOUT: the scout's evaluation when its ticket holds (2: compared with the render thread's own); not in a
	// speculation (the scout follows the render thread's submissions).
#if KYTY_CX_SCOUT
	const auto* scout = Scout::Running() && spec_state == nullptr
	                        ? TableScoutTicket(pair, user_data, stages, m_context, static_cast<const Scout::Ticket*>(m_table_ticket))
	                        : nullptr;
	if (scout != nullptr && (Scout::Mode() == 1 || Scout::Mode() == 3)) {
		TableScoutApply(table, pair, *scout, stages);
	} else {
		const bool evaluated = TableEvaluate(pair, user_data);
		if (evaluated) TableGather(pair, user_data);
		if (scout != nullptr) TableScoutVerify(table, pair, *scout, stages, evaluated);
		if (!evaluated)
			return LiveCounters::Add(LiveCounters::TableDrawNative), Spec::Refuse("table: evaluation"), TableResult::Native;
	}
#else
	{
		const bool evaluated = TableEvaluate(pair, user_data);
		if (evaluated) TableGather(pair, user_data);
		if (!evaluated)
			return LiveCounters::Add(LiveCounters::TableDrawNative), Spec::Refuse("table: evaluation"), TableResult::Native;
	}
#endif
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1e, 2);
	auto* set = TableSet(buffer, pair, &variant, &draw_state);
	if (spec_state != nullptr && set != nullptr)
		spec_state->NoteSet(reinterpret_cast<uint64_t>(static_cast<VkDescriptorSet>(set->set)));
	if (set == nullptr) return LiveCounters::Add(LiveCounters::TableNoSet), Spec::Refuse("table: no set"), TableResult::Native;

	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1e, 3);
	// Emission, as NativeXprEmit's: indices and arguments are read by the GPU; lookups may restart the
	// scheduler, so the command buffer handle is taken after them.
	auto& buffer_cache = m_context.GetBufferCache();
	// (KYTY_SPEC_DRAW_INDIRECT: a draw of vertices of arguments the GPU reads, VkDrawIndirectCommand; obtained as a run's.)
	const bool     vertex_arguments = direct != nullptr && direct->indirect;
	const uint64_t stride = vertex_arguments ? sizeof(vk::DrawIndirectCommand) : sizeof(vk::DrawIndexedIndirectCommand);
	// A tracker region's epoch, read once in the draw (its buffers mostly share one: the line is another core's).
	std::array<std::pair<uint64_t, uint64_t>, 4> region_epochs;
	region_epochs.fill({UINT64_MAX, 0});
	const auto cpu_epoch = [&](uint64_t address, uint64_t size) {
		const auto region = address / TRACKER_REGION_SIZE;
		if (region != (address + size - 1) / TRACKER_REGION_SIZE) return buffer_cache.CpuModificationEpoch(address, size);
		auto& known = region_epochs[region & 3u];
		if (known.first != region) known = {region, buffer_cache.CpuModificationEpoch(address, size)};
		return known.second;
	};
	const auto obtain = [&](TableXpr::HeldBuffer& held, uint64_t address, uint64_t size,
	                        auto&& prepare) -> std::pair<Buffer*, uint64_t> {
		const auto* spec = spec_state; // (its guest memory writes wait for its commit: ObtainBuffer decides)
		if (held.buffer != nullptr && address >= held.begin && address + size <= held.end &&
		    held.registration == buffer_cache.RegistrationEpoch() && held.cpu_epoch != 0 &&
		    cpu_epoch(held.begin, held.end - held.begin) == held.cpu_epoch &&
		    (spec == nullptr || !spec->JournalIntersects(address, size))) {
			if (spec != nullptr) spec_state->NoteBufferRange(address, size);
			return {held.buffer, held.buffer->Offset(address)};
		}
		held = {};
		LiveCensus::Scope miss(LiveCensus::DrawPhase, 0x7ab1e, &held == &table.args ? 10 : 11);
		prepare();
		const auto registration_before = buffer_cache.RegistrationEpoch(); // (a speculation's: before its lookup)
		const auto result = buffer_cache.ObtainBuffer(address, size, false);
		region_epochs.fill({UINT64_MAX, 0}); // (its uploads may have moved them)
		const auto owner  = spec_state != nullptr ? buffer_cache.RegisteredBuffer(address, size)
		                                               : buffer_cache.FindBuffer(address, size);
		if (result.first != nullptr && buffer_cache.GetBufferIfLive(owner) == result.first) {
			// The buffer within the tracker region of the address (one region's epoch proves it) when none of it is
			// CPU-dirty, else the range. (The epoch before the dirty check: a write after it moves the epoch.)
			const auto region = address & ~(TRACKER_REGION_SIZE - 1);
			auto       begin  = std::max(result.first->CpuAddress(), region);
			auto       end    = std::min(result.first->CpuAddress() + result.first->Size(), region + TRACKER_REGION_SIZE);
			if (address + size <= end) {
				const auto epoch = buffer_cache.CpuModificationEpoch(begin, end - begin);
				if (buffer_cache.IsRegionCpuModified(begin, end - begin)) {
					begin = address;
					end   = address + size;
				}
				if (epoch != 0 && !buffer_cache.IsRegionCpuDirty(begin, end - begin))
					held = {begin, end, epoch, spec_state != nullptr ? registration_before : buffer_cache.RegistrationEpoch(),
					        result.first};
			}
		}
		return result;
	};
	std::array<vk::DeviceAddress, 2> blocks {};
	bool                             writes = false;
	// (Before the blocks, which it does not depend on (the slots TableEvaluate read): the entries TableGather asked
	// for meanwhile arrive.)
	TablePrepareBda(pair, user_data);
	// The blocks take each buffer's device address as it resolves: a later lookup that registers a buffer (a merge
	// replaces an earlier one) leaves an earlier address stale, where the normal path binds after all its lookups
	// (RebindBuffers). Such a draw takes the normal path; the next use finds the buffers registered.
	const auto registration = buffer_cache.RegistrationEpoch();
	if (!TableBlocks(pair, user_data, blocks, &writes))
		return LiveCounters::Add(LiveCounters::TableDrawNative), Spec::Refuse("table: blocks"), TableResult::Native;
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1e, 9);
	std::array<std::pair<Buffer*, uint64_t>, 64> arguments;
	std::array<uint32_t, 64>                     group_sizes;
	uint32_t                                     groups = 0;
	for (size_t first = 0; first < commands.size();) {
		size_t count = 1;
		while (first + count < commands.size() && commands[first + count] == commands[first] + count * stride) ++count;
		const auto [owner, offset] = obtain(table.args, commands[first], count * stride, [] {});
		if (owner == nullptr) return TableResult::Native;
		arguments[groups]   = {owner, offset};
		group_sizes[groups] = static_cast<uint32_t>(count);
		++groups;
		first += count;
	}
	auto&      emit       = cache.emit;
	auto&      dynamic    = m_context.GetCommandScheduler().GetDynamicState();
	const bool indexed    = direct == nullptr || direct->indexed; // (a draw of vertices binds no indices)
	const bool same_index = !indexed || (emit.generation == dynamic.Generation() &&
	                                     emit.index_base == index_base && emit.index_bytes == index_bytes &&
	                                     emit.index_type == index_type);
	std::pair<Buffer*, uint64_t> indices {};
	if (!same_index) {
		indices = obtain(table.indices[(index_base * 0x9e3779b97f4a7c15ull) >> 51u], index_base, index_bytes, [&] {
			if (index_bytes <= BufferCache::CACHING_PAGESIZE && buffer_cache.IsRegionCpuModified(index_base, index_bytes) &&
			    !buffer_cache.IsRegionGpuModified(index_base, index_bytes))
				buffer_cache.EnsureBufferContents(index_base, index_bytes);
		});
		if (indices.first == nullptr) return TableResult::Native;
	}
	if (buffer_cache.RegistrationEpoch() != registration)
		return LiveCounters::Add(LiveCounters::TableRegistered), TableResult::Native;
	// (A speculation's lookups and preparation refuse without failing: nothing is recorded for a refused draw.)
	if (spec_state != nullptr && spec_state->refused != nullptr) return TableResult::Native;
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1e, 4);
	if (buffer.IsInvalid()) return Spec::Refuse("table: buffer"), TableResult::Native;
	auto       vk_buffer    = buffer.Handle();
	const bool same_command = emit.generation == dynamic.Generation();
	// The targets and the set's images as the last table draw in this command buffer left them (the same state and
	// set), no image's state, registration, metadata or bytes changed since: in place, nothing to check or transit.
	auto&      texture_cache = m_context.GetTextureCache();
	const auto images_epoch  = texture_cache.ImagesEpoch();
	const bool own           = spec_state == nullptr;
	// (A set with an image the draw reads and writes is transited for every draw: KYTY_DRAW_READ_AFTER_WRITE.)
	const bool read_after_write =
	    set->read_after_write && kyty_local_draw_read_after_write.load(std::memory_order_relaxed) != 0;
	const bool in_place      = same_command && emit.state == &draw_state && table.last_set == set &&
	                      table.images_epoch == images_epoch && own && !read_after_write;
	// KYTY_TABLE_SERIALS: the checks again only when a serial sum moved (1), or as before with the sums counted (2).
	const auto serials = own ? TableSerialsMode() : 0u;
	(void)serials;
	if (!in_place) {
		// The same for the targets apart, as this draw state's last emission left them, and the set's images, as its
		// last transit under this state did (the passes alternate materials: another set or state in between).
		const bool epoch_targets = !own || draw_state.emitted_epoch != images_epoch;
		const auto targets_sum   = serials != 0 ? TableTargetSerials(draw_state) : 0;
		const bool sum_targets   = !own || draw_state.emitted_serials != targets_sum;
		const bool due_targets   = serials == 1 ? sum_targets : epoch_targets;
		if (serials != 0 && epoch_targets != sum_targets)
			LiveCounters::Add(sum_targets ? LiveCounters::SerialsExtra : LiveCounters::SerialsSkipped);
		if (due_targets && !NativeXprEmitTargets(draw_state, vk_buffer, set)) {
			if (serials == 2 && !sum_targets) TableSerialsUnsafe("targets refused");
			return LiveCounters::Add(LiveCounters::TableDrawNative), Spec::Refuse("table: targets"), TableResult::Native;
		}
		if (serials == 2 && due_targets && !sum_targets && TableTargetSerials(draw_state) != targets_sum)
			TableSerialsUnsafe("targets moved");
		// The set's images in their layouts (a DCC image's pending fast clear applied first).
		const bool epoch_transit = !own || set->transit_epoch != images_epoch || set->transit_state != &draw_state;
		const auto set_sum       = serials != 0 ? TableSetSerials(*set) : 0;
		const bool sum_transit   = !own || set->transit_serials != set_sum ||
		                         ((set->transit_state != &draw_state || set->transit_capture != draw_state.capture_id) &&
		                          !TableSetApart(*set, draw_state));
		const bool due_transit   = (serials == 1 ? sum_transit : epoch_transit) || read_after_write;
		if (serials != 0 && epoch_transit != sum_transit)
			LiveCounters::Add(sum_transit ? LiveCounters::SerialsExtra : LiveCounters::SerialsSkipped);
		if (due_transit && !TableTransit(*set, vk_buffer, &draw_state)) {
			if (serials == 2 && !sum_transit && !read_after_write) TableSerialsUnsafe("transit refused");
			return LiveCounters::Add(LiveCounters::TableDrawNative), Spec::Refuse("table: transit"), TableResult::Native;
		}
		if (serials == 2 && due_transit && !sum_transit && !read_after_write && TableSetSerials(*set) != set_sum)
			TableSerialsUnsafe("transit moved");
		// (Neither moves what the other placed: a target is no image the set holds as a texture.)
		if (own) {
			draw_state.emitted_epoch = set->transit_epoch = texture_cache.ImagesEpoch();
			set->transit_state       = &draw_state;
			set->transit_capture     = draw_state.capture_id;
			if (serials != 0) {
				draw_state.emitted_serials = due_targets || due_transit ? TableTargetSerials(draw_state) : targets_sum;
				set->transit_serials       = due_targets || due_transit ? TableSetSerials(*set) : set_sum;
			}
		}
	}
	if (variant.colors != draw_state.rendering.num_color_attachments ||
	    variant.has_depth != draw_state.rendering.depth_stencil_attachment.has_depth ||
	    variant.has_stencil != draw_state.rendering.depth_stencil_attachment.has_stencil) {
		// (The state of this key was captured again for another rendering since the variant's pipeline was made: the
		// next store of the pair makes the pipeline again, for the state as it is now.)
		variant.pipeline = nullptr;
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
			std::printf("Table: VS 0x%llx PS 0x%llx variant made for %u color attachments, its draw state has %u\n",
			            static_cast<unsigned long long>(pair.vs), static_cast<unsigned long long>(pair.ps), variant.colors,
			            draw_state.rendering.num_color_attachments);
		return Spec::Refuse("table: variant colors"), TableResult::Native;
	}
	buffer.BeginRendering(draw_state.rendering);
	const bool query_bias = m_context.GetOcclusionQueries().Counting();
	if (!same_command || emit.state != &draw_state || emit.query_bias != query_bias)
		NativeXprEmitDynamic(cache, vk_buffer, draw_state, same_command, query_bias);
	const auto& pipeline = *variant.pipeline;
	if (!same_command || emit.pipeline != &pipeline)
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!same_command || emit.pipeline != &pipeline || table.last_set != set)
		vk_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline_layout, 0, 1, &set->set, 0,
		                             nullptr);
	// Each stage's user data and the pixel stage's LOD statistics words, where its push data starts.
	ShaderRecompiler::IR::PushData push_data;
	uint32_t                       push_dwords = 0; // the stages' shader data end there: nothing after it is pushed
	for (uint32_t st = 0; st < stages; ++st) {
		const auto& program  = *pair.programs[st];
		const auto& bindings = program.bindings;
		if (!bindings.UsesPushData()) continue;
		push_dwords = std::max(push_dwords, bindings.push_data_start_dword + bindings.ShaderDataDwords());
		for (size_t i = 0; i < program.info.user_data_registers.size(); ++i) {
			const auto index = program.info.user_data_registers[i] - program.user_data_base;
			push_data.dwords[bindings.push_data_start_dword + i] = index < user_data[st].size() ? user_data[st][index] : 0;
		}
		if (program.stage == ShaderType::Pixel)
			for (size_t i = 0; i < table.lod_stats.size(); ++i)
				push_data.dwords[bindings.push_data_start_dword + bindings.MipStatsDword() + i] = table.lod_stats[i];
		push_data.dwords[bindings.push_data_start_dword + bindings.TableBlockDword()]     = static_cast<uint32_t>(blocks[st]);
		push_data.dwords[bindings.push_data_start_dword + bindings.TableBlockDword() + 1] = static_cast<uint32_t>(blocks[st] >> 32u);
	}
	if (push_dwords != 0)
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_stages, 0, push_dwords * sizeof(uint32_t),
		                        push_data.dwords.data());
	if (!same_index) vk_buffer.bindIndexBuffer(indices.first->Handle(), indices.second, index_type);
	if (direct != nullptr && !vertex_arguments) {
		// This draw's offsets, as the normal path's ResolveVertexOffset/ResolveInstanceOffset give them: the index
		// offset register, else (a vertex program that fetches) the user data its fetch adds.
		const auto& vertex = *pair.programs[0];
		const auto  word   = [&](int32_t sgpr) {
			const auto index = int64_t {sgpr} - vertex.user_data_base;
			return index >= 0 && index < static_cast<int64_t>(user_data[0].size()) ? user_data[0][index] : 0u;
		};
		const uint32_t index_offset   = buffer.GetUserConfig().GetIndexOffset();
		const uint32_t vertex_offset  = variant.fetch_embedded && index_offset == 0 ? word(vertex.info.vertex_offset_sgpr)
		                                                                            : index_offset;
		const uint32_t first_instance = variant.fetch_embedded ? word(vertex.info.instance_offset_sgpr) : 0u;
		if (indexed)
			vk_buffer.drawIndexed(direct->index_count, direct->instance_count, 0,
			                      static_cast<int32_t>(vertex_offset) + direct->base_vertex, first_instance);
		else
			vk_buffer.draw(direct->index_count, direct->instance_count, vertex_offset, first_instance);
	}
	// (A draw of vertices of GPU-read arguments: their first vertex and instance from them, as the normal path's
	// DrawAuto with gpu_args gives them (DrawOffsetSource::IndirectArgs), not from the index offset or user data.)
	for (uint32_t group = 0; group < groups; ++group) {
		if (vertex_arguments)
			vk_buffer.drawIndirect(arguments[group].first->Handle(), arguments[group].second, group_sizes[group],
			                       static_cast<uint32_t>(stride));
		else
			vk_buffer.drawIndexedIndirect(arguments[group].first->Handle(), arguments[group].second, group_sizes[group],
			                              static_cast<uint32_t>(stride));
	}
	// As the normal path's: shaders that write buffers end the rendering, and a barrier orders their writes.
	if (writes) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader);
	}
	emit.generation  = dynamic.Generation();
	emit.state       = &draw_state;
	emit.pipeline    = &pipeline;
	emit.query_bias  = query_bias;
	draw_state.Use(m_context.FrameNumber());
	if (indexed) {
		emit.index_base  = index_base;
		emit.index_bytes = index_bytes;
		emit.index_type  = index_type;
	}
	table.last_pair    = &pair;
	table.last_variant = static_cast<size_t>(&variant - pair.variants.data());
	table.last_state   = &draw_state;
	table.last_set     = set;
	table.images_epoch = texture_cache.ImagesEpoch();
	LiveCounters::Add(LiveCounters::TableDraws, std::max<size_t>(commands.size(), 1));
	return TableResult::Drawn;
}

// The pair's runs (TablePair::Run): the slots its stages evaluate, in reads of consecutive slots, by level.
void RenderExecutor::TableRuns(TablePair& pair) {
	using Kind  = ShaderRecompiler::IR::TablePlan::Operand::Kind;
	auto& table = *m_table_xpr;
	std::array<TableFlatStage, 2> flat_forms; // (packed into the pair: TablePack)
	for (uint32_t st = 0; st < 2; ++st) {
		if (pair.programs[st] == nullptr) continue;
		const auto&           plan = pair.programs[st]->table_plan;
		std::vector<uint32_t> levels(plan.slots.size(), 0);
		const auto            same = [](const ShaderRecompiler::IR::TablePlan::Operand& a,
		                     const ShaderRecompiler::IR::TablePlan::Operand& b) {
			return a.kind == b.kind && a.value == b.value;
		};
		pair.runs[st].clear();
		for (uint32_t first = 0; first < plan.slots.size();) {
			if (plan.cpu[first] == 0) {
				++first;
				continue;
			}
			const auto& slot  = plan.slots[first];
			uint32_t    count = 1;
			while (first + count < plan.slots.size() && plan.cpu[first + count] != 0 &&
			       same(plan.slots[first + count].low, slot.low) && same(plan.slots[first + count].high, slot.high) &&
			       same(plan.slots[first + count].offset, slot.offset) &&
			       plan.slots[first + count].bounded == slot.bounded && same(plan.slots[first + count].records, slot.records) &&
			       plan.slots[first + count].immediate == slot.immediate + static_cast<int32_t>(4 * count))
				++count;
			uint32_t level = 0;
			for (const auto* operand: {&slot.low, &slot.high, &slot.offset, &slot.records})
				if (operand->kind == Kind::Slot) level = std::max(level, levels[operand->value] + 1);
			std::fill_n(levels.begin() + first, count, level);
			pair.runs[st].push_back({slot, first, count, level});
			first += count;
		}
		std::ranges::stable_sort(pair.runs[st], {}, &TablePair::Run::level);
		table.addresses.resize(std::max(table.addresses.size(), pair.runs[st].size()));
		// The flat form: a user data register by its index (TableEvaluate takes it where the stage's user data hold it,
		// value() reads one beyond them as 0: one of 64 and up is the constant 0), a slot by its index, an immediate
		// from the constants (the first is 0).
		const auto& program = *pair.programs[st];
		auto&       flat    = flat_forms[st];
		flat.constants.push_back(0);
		const auto flat_of = [&](const ShaderRecompiler::IR::TablePlan::Operand& operand) -> TableFlatStage::Operand {
			if (operand.kind == Kind::Slot) return {operand.value, 1};
			if (TableCodeOperand(operand)) {
				flat.constants.push_back(TableCodeWord(operand, st == 0 ? pair.vs : pair.ps));
				return {static_cast<uint32_t>(flat.constants.size() - 1), 2};
			}
			if (operand.kind == Kind::UserData) {
				const auto index = operand.value - program.user_data_base;
				if (index >= 64) return {0, 2};
				flat.user_words = std::max(flat.user_words, index + 1);
				return {index, 0};
			}
			flat.constants.push_back(operand.value);
			return {static_cast<uint32_t>(flat.constants.size() - 1), 2};
		};
		const auto& runs = pair.runs[st];
		for (size_t i = 0; i < runs.size(); ++i) {
			const auto& run = runs[i];
			flat.runs.push_back({{flat_of(run.read.low), flat_of(run.read.high), flat_of(run.read.offset)},
			                     run.read.immediate, run.first, run.count, flat_of(run.read.records), run.read.bounded});
			if (i + 1 == runs.size() || runs[i + 1].level != run.level) flat.level_ends.push_back(static_cast<uint32_t>(i + 1));
		}
		for (const auto& descriptor: plan.images) {
			flat.image_counts.push_back(descriptor.count);
			for (uint32_t w = 0; w < descriptor.count; ++w) flat.image_words.push_back(flat_of(descriptor.words[w]));
		}
		for (const auto& descriptor: plan.samplers)
			for (uint32_t w = 0; w < descriptor.count; ++w) flat.image_words.push_back(flat_of(descriptor.words[w]));
		for (const auto& words: plan.buffers)
			for (const auto& word: words) flat.buffer_words.push_back(flat_of(word));
	}
	TablePack(pair, flat_forms);
}

// The pair's flat forms packed into one block (TablePair::packed): a stage whose operand indices, counts or levels do not
// fit their packed widths is left invalid (evaluated by its plan's operands).
void RenderExecutor::TablePack(TablePair& pair, const std::array<TableFlatStage, 2>& flat_forms) {
	using Pair = TablePair;
	pair.packed       = {};
	pair.packed_block = nullptr;
	pair.packed_at    = nullptr;
	pair.packed_bytes = 0;
	const auto operand = [](const TableFlatStage::Operand& from, bool& fits) -> uint16_t {
		if (from.base > 2 || from.index >= 0x4000u) fits = false;
		return static_cast<uint16_t>(from.base << 14u | (from.index & 0x3fffu));
	};
	const auto align = [](size_t at, size_t to) { return (at + to - 1) / to * to; };
	// (Layout per stage: runs, constants, level ends, image words, buffer words, image counts.)
	std::array<std::array<size_t, 6>, 2> offsets {};
	size_t                               bytes = 0;
	std::array<bool, 2>                  fits {};
	for (uint32_t st = 0; st < 2; ++st) {
		if (pair.programs[st] == nullptr) continue;
		const auto& flat = flat_forms[st];
		fits[st]         = flat.runs.size() < 0x10000 && flat.image_words.size() < 0x10000;
		for (const auto end: flat.level_ends) fits[st] = fits[st] && end < 0x10000;
		for (const auto count: flat.image_counts) fits[st] = fits[st] && count < 0x100;
		for (const auto& run: flat.runs) fits[st] = fits[st] && run.first < 0x10000 && run.count < 0x10000;
		if (!fits[st]) continue;
		bytes          = align(bytes, 64);
		offsets[st][0] = bytes;
		bytes += flat.runs.size() * sizeof(Pair::PackedRun);
		offsets[st][1] = bytes;
		bytes += flat.constants.size() * sizeof(uint32_t);
		offsets[st][2] = bytes;
		bytes += flat.level_ends.size() * sizeof(uint16_t);
		offsets[st][3] = bytes;
		bytes += flat.image_words.size() * sizeof(uint16_t);
		offsets[st][4] = bytes;
		bytes += flat.buffer_words.size() * sizeof(uint16_t);
		offsets[st][5] = bytes;
		bytes += flat.image_counts.size();
	}
	if (bytes == 0) return;
	pair.packed_block = std::make_unique<std::byte[]>(bytes + 64);
	auto* base        = reinterpret_cast<std::byte*>(align(reinterpret_cast<uintptr_t>(pair.packed_block.get()), 64));
	pair.packed_at    = base;
	pair.packed_bytes = static_cast<uint32_t>(bytes);
	for (uint32_t st = 0; st < 2; ++st) {
		if (!fits[st]) continue;
		const auto& flat    = flat_forms[st];
		auto&       packed  = pair.packed[st];
		bool        operand_fits = true;
		auto*       runs    = reinterpret_cast<Pair::PackedRun*>(base + offsets[st][0]);
		for (size_t i = 0; i < flat.runs.size(); ++i) {
			const auto& run = flat.runs[i];
			runs[i]         = {.address   = {operand(run.address[0], operand_fits), operand(run.address[1], operand_fits),
			                                 operand(run.address[2], operand_fits)},
			                   .records   = run.bounded ? operand(run.records, operand_fits) : Pair::PackedRun::NotBounded,
			                   .immediate = run.immediate,
			                   .first     = static_cast<uint16_t>(run.first),
			                   .count     = static_cast<uint16_t>(run.count)};
			if (run.bounded && runs[i].records == Pair::PackedRun::NotBounded) operand_fits = false;
		}
		auto* constants = reinterpret_cast<uint32_t*>(base + offsets[st][1]);
		std::copy(flat.constants.begin(), flat.constants.end(), constants);
		auto* level_ends = reinterpret_cast<uint16_t*>(base + offsets[st][2]);
		for (size_t i = 0; i < flat.level_ends.size(); ++i) level_ends[i] = static_cast<uint16_t>(flat.level_ends[i]);
		auto* image_words = reinterpret_cast<uint16_t*>(base + offsets[st][3]);
		for (size_t i = 0; i < flat.image_words.size(); ++i) image_words[i] = operand(flat.image_words[i], operand_fits);
		auto* buffer_words = reinterpret_cast<uint16_t*>(base + offsets[st][4]);
		for (size_t i = 0; i < flat.buffer_words.size(); ++i) buffer_words[i] = operand(flat.buffer_words[i], operand_fits);
		auto* image_counts = reinterpret_cast<uint8_t*>(base + offsets[st][5]);
		for (size_t i = 0; i < flat.image_counts.size(); ++i) image_counts[i] = static_cast<uint8_t>(flat.image_counts[i]);
		packed = {.runs         = {runs, flat.runs.size()},
		          .level_ends   = {level_ends, flat.level_ends.size()},
		          .image_words  = {image_words, flat.image_words.size()},
		          .buffer_words = {buffer_words, flat.buffer_words.size()},
		          .image_counts = {image_counts, flat.image_counts.size()},
		          .constants    = {constants, flat.constants.size()},
		          .user_words   = flat.user_words,
		          .valid        = operand_fits};
	}
}

// After a normal draw a table try asked to store: the pair's table programs (compiled by a worker of the program cache)
// and this draw state's pipeline, and the draw state (NativeXprCaptureState).
void RenderExecutor::TableStore(CommandBuffer& buffer, const DrawRenderState& state, vk::PrimitiveTopology topology,
                                bool primitive_restart_enable, const RenderState& rendering,
                                const DynamicState& dynamic_state, bool storable) {
	auto& table = OwnTable();
	auto* pair  = std::exchange(table.store, nullptr);
	if (pair == nullptr) return;
	Spec::Refuse("store: called");
	const auto& sh = buffer.GetShaders();
	if (sh.GetVs().es_regs.data_addr != pair->vs || sh.GetPs().ps_regs.data_addr != pair->ps)
		return Spec::Refuse("store: other pair"), void();
	const auto refuse = [&](const char* reason) {
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
			std::printf("Table: VS 0x%llx PS 0x%llx refused: %s\n", static_cast<unsigned long long>(pair->vs),
			            static_cast<unsigned long long>(pair->ps), reason);
		pair->refused = true;
		Spec::Refuse("store: pair refused");
	};
	if (!storable) return refuse("a draw table programs cannot make (vertex buffers, draw data, host indices)");
	if (state.VertexStages().size() != 1 || state.vertex_info[0].stage.module == nullptr ||
	    state.vertex_info[0].logical_stage == ShaderType::Mesh || state.vertex_info[0].user_clip_planes != 0 ||
	    (state.ps_active && state.ps_input_info.stage.module == nullptr))
		return refuse("stages");
	auto&       pipelines = m_context.GetPipelineCache();
	const auto& ctx       = buffer.GetRegisters();
	// The stages' static input, which the pair's programs are compiled for: the normal draw's own, both stages' words
	// (BuildStageStaticKey assigns its vector).
	std::vector<uint32_t> key;
	BuildStageStaticKey(state.vertex_info[0], key);
	if (state.ps_active) {
		std::vector<uint32_t> pixel_key;
		BuildStageStaticKey(state.ps_input_info, pixel_key);
		key.insert(key.end(), pixel_key.begin(), pixel_key.end());
	}
	const auto static_key = XXH3_64bits_withSeed(
	    key.data(), key.size() * sizeof(uint32_t),
	    state.vertex_info[0].stage.module->info.shader_hash * 31u +
	        (state.ps_active ? state.ps_input_info.stage.module->info.shader_hash : 0u));
	if (pair->programs[0] == nullptr) {
		// The table programs, for the specializations of the normal draw's (a worker compiles them: Pending until
		// it is done, and the next store of the pair asks again). The push data cursor runs as the normal
		// lookup's does: the pixel stage first, then the vertex stage.
		ShaderVertexInputInfo vertex_scratch {};
		ShaderPixelInputInfo  pixel_scratch {};
		const auto vertex_params = PrepareProgram(sh.GetVs(), ctx, buffer.GetUserConfig(), vertex_scratch);
		ShaderParams pixel_params;
		if (state.ps_active)
			pixel_params = PrepareProgram(sh.GetPs(), ctx.GetShaderRegisters(),
			                              TableDrawExportMapping(m_context.GetGraphics(), ctx, state.mrt_mask), pixel_scratch);
		const auto [vertex_plan, vertex_specialization] = pipelines.ProgramPlanOf(*state.vertex_info[0].stage.module);
		const auto [pixel_plan, pixel_specialization] =
		    state.ps_active ? pipelines.ProgramPlanOf(*state.ps_input_info.stage.module)
		                    : std::pair<const ShaderRecompiler::IR::ResourcePlan*,
		                                const ShaderRecompiler::IR::ResourceSpecialization*> {nullptr, nullptr};
		if (vertex_plan == nullptr || vertex_specialization == nullptr ||
		    (state.ps_active && (pixel_plan == nullptr || pixel_specialization == nullptr))) {
			// DSP-INSTR
			char why[160];
			std::snprintf(why, sizeof(why), "no resource plan (VS plan %p id %llx, PS plan %p id %llx)",
			              static_cast<const void*>(vertex_plan),
			              static_cast<unsigned long long>(state.vertex_info[0].stage.module->id),
			              static_cast<const void*>(pixel_plan),
			              static_cast<unsigned long long>(state.ps_active ? state.ps_input_info.stage.module->id : 0));
			return refuse(why);
		}
		// The buffers the pair saw change format (TablePair::portable) are specialized for none: their programs decode
		// the V#'s at run time.
		const auto portable = [](const ShaderRecompiler::IR::ResourceSpecialization& specialization, uint32_t mask) {
			auto result = specialization;
			for (size_t i = 0; i < result.buffers.size() && i < 32; ++i) {
				if (((mask >> i) & 1u) == 0) continue;
				result.buffers[i].descriptor_format  = Prospero::BufferFormat::kInvalid;
				result.buffers[i].descriptor_swizzle = DstSel(4, 5, 6, 7);
			}
			return result;
		};
		const auto vertex_portable = portable(*vertex_specialization, pair->portable[0]);
		const auto pixel_portable  = state.ps_active ? portable(*pixel_specialization, pair->portable[1])
		                                             : ShaderRecompiler::IR::ResourceSpecialization {};
		uint32_t cursor = ShaderRecompiler::IR::PushData::GraphicsDrawDwords(false, false, 0);
		PipelineCache::TableProgram pixel {};
		if (state.ps_active)
			pixel = pipelines.GetPixelProgramTable(pixel_params, state.ps_input_info, pixel_portable, cursor);
		const auto vertex = pipelines.GetVertexProgramTable(vertex_params, state.vertex_info[0], vertex_portable, cursor);
		using Status = PipelineCache::TableStatus;
		if (vertex.status == Status::Refused || (state.ps_active && pixel.status == Status::Refused)) {
			const auto* why = vertex.status == Status::Refused ? vertex.refusal : pixel.refusal;
			return refuse(why != nullptr ? why->c_str() : "table mode cannot serve it");
		}
		if (vertex.status == Status::Pending || (state.ps_active && pixel.status == Status::Pending)) {
			pair->pending = true;
			// (The pair stores again once the program is there: request_store defers meanwhile.)
			pair->waiting = vertex.status == Status::Pending ? vertex.waiting : pixel.waiting;
			Spec::Refuse("store: programs pending");
			return;
		}
		const auto serves = [](const ShaderModule* module) {
			return module != nullptr && module->info.table_mode && module->info.bindings.UsesPushData() &&
			       !module->info.has_address_writes && !module->info.info.uses_gds;
		};
		if (!serves(vertex.module) || (state.ps_active && !serves(pixel.module))) return refuse("a program table mode cannot draw");
		pair->pending     = false;
		Spec::Refuse("store: programs set");
		pair->tables      = {vertex.module, state.ps_active ? pixel.module : nullptr};
		pair->programs    = {&vertex.module->info, state.ps_active ? &pixel.module->info : nullptr};
		pair->plans       = {vertex_plan, pixel_plan};
		pair->static_key  = static_key;
		pair->has_pixel   = state.ps_active;
		pair->code_hashes = {ShaderHashAt(pair->vs), state.ps_active ? ShaderHashAt(pair->ps) : 0};
		TableRuns(*pair);
#if KYTY_CX_SCOUT
		TableScoutPublish(*pair); // (the scout evaluates this pair's draws from its flat form)
#endif
	}
	auto variant = std::ranges::find(pair->variants, table.store_state, &TableVariant::state);
	if (variant == pair->variants.end()) variant = pair->variants.insert(variant, {.state = table.store_state});
	if (static_key != pair->static_key || state.ps_active != pair->has_pixel) {
		// Another specialization (the pixel program's export of another draw state's targets): this state's draws
		// take the normal path.
		variant->refused = true;
		Spec::Refuse("store: variant refused (static key)");
		return;
	}
	// The pipeline: the normal draw's state with the table programs, native bindings.
	DrawRenderState table_state = state;
	table_state.vertex_info[0].stage = {.module = pair->tables[0], .resources = &g_table_no_resources};
	if (state.ps_active) table_state.ps_input_info.stage = {.module = pair->tables[1], .resources = &g_table_no_resources};
	const auto* pipeline = pipelines.GetGraphicsPipeline(buffer, table_state, *m_draw_targets, NativeXprAsyncPipelines(), true);
	if (pipeline != nullptr && (!pipeline->native_bindings || pipeline->uses_push_descriptors)) {
		variant->refused = true;
		Spec::Refuse("store: variant refused (bindings)");
		return;
	}
	Spec::Refuse(pipeline != nullptr ? "store: pipeline ready" : "store: pipeline null (async)");
	if (pipeline == nullptr) pair->waiting = pipelines.PipelineWaiting(); // (asked again once it is done)
	variant->pipeline       = pipeline; // (null while a worker creates it: a later draw's store takes it)
	variant->colors         = rendering.num_color_attachments;
	variant->has_depth      = rendering.depth_stencil_attachment.has_depth;
	variant->has_stencil    = rendering.depth_stencil_attachment.has_stencil;
	variant->fetch_embedded = state.vertex_info[0].has_dynamic_vertex_fetch;
	NativeXprCaptureState(state, rendering, table.store_state, dynamic_state);
}

// The table path's entry from the command processor (this tree's counterpart of his NativeXprTry call, which tries the
// table draw first): `args_addr` the GPU-side DrawIndexedIndirectCommand of an indirect draw, or `direct` a direct
// indexed draw; `clean`: no graphics register but user data changed since the draw before (DrawStateObserver), which
// KYTY_TABLE_XPR=2 continues from the last table draw. False: the normal path draws (storing a table variant when
// the try asked, TableStore).
namespace {
// The switches of his performance-switches.h the table path reads, from the environment once.
void TableLoadSwitches() {
	static const bool loaded = [] {
		const auto load = [](const char* name, volatile std::atomic_uint32_t& value) {
			if (const char* text = std::getenv(name); text != nullptr && text[0] != 0)
				value.store(static_cast<uint32_t>(std::strtoul(text, nullptr, 0)), std::memory_order_relaxed);
		};
		load("KYTY_TABLE_XPR", kyty_local_table_xpr_mode);
		// (Table dispatches are on in Demon's Souls, where they were measured: KYTY_TABLE_DISPATCH=0/1 forces them.)
		if (::Loader::DemonsSoulsTitle::IsSupportedGame()) kyty_local_table_dispatch_mode.store(1, std::memory_order_relaxed);
		load("KYTY_TABLE_DISPATCH", kyty_local_table_dispatch_mode);
		load("KYTY_TABLE_INDIRECT", kyty_local_table_indirect_mode);
		load("KYTY_TABLE_STORE_BUDGET", kyty_local_table_store_budget);
		load("KYTY_TABLE_SET_PROOFS", kyty_local_table_set_proofs);
		load("KYTY_TABLE_MRU", kyty_local_table_mru);
		load("KYTY_TABLE_TARGET_READ", kyty_local_table_target_read);
		load("KYTY_TABLE_GATHER_MEMO", kyty_local_table_gather_memo);
		load("KYTY_TABLE_MEMO", kyty_local_table_memo);
		load("KYTY_TABLE_MEMO_VERIFY", kyty_local_table_memo_verify);
		load("KYTY_TABLE_SERIALS", kyty_local_table_serials);
		load("KYTY_DRAW_READ_AFTER_WRITE", kyty_local_draw_read_after_write);
		return true;
	}();
	(void)loaded;
}
uint32_t TableXprMode() {
	TableLoadSwitches();
	return kyty_local_table_xpr_mode.load(std::memory_order_relaxed);
}
uint32_t TableDispatchMode() {
	TableLoadSwitches();
	return kyty_local_table_dispatch_mode.load(std::memory_order_relaxed);
}
} // namespace

namespace {
uint64_t TableLikelyTag(const HW::Shader& shaders) noexcept {
	return ((shaders.GetVs().es_regs.data_addr * 0x9e3779b97f4a7c15ull) ^ (shaders.GetPs().ps_regs.data_addr * 0xc2b2ae3d27d4eb4full)) | 1ull;
}
} // namespace

uint32_t RenderExecutor::TableHint(const HW::Shader& shaders) const noexcept {
	if (TableXprMode() == 0) return 0;
	const auto tag  = TableLikelyTag(shaders);
	const auto slot = (tag >> 40u) & (m_table_drawn.size() - 1);
	if (m_table_drawn[slot].load(std::memory_order_relaxed) != tag) return 0;
	return m_table_mixed_until[slot].load(std::memory_order_relaxed) < m_context.FrameNumber() ? 1u : 3u;
}

// Whether the last dispatch of the compute stage's code went through its table program (TableDispatch): the resolve
// thread makes its table evaluation ahead instead of resolving it (KYTY_SCOUT=3), and resolves it all the same while a
// dispatch of that code took the normal path lately (3).
uint32_t RenderExecutor::TableDispatchHint(const HW::ComputeShaderInfo& cs) const noexcept {
	if (TableXprMode() == 0 || TableDispatchMode() == 0) return 0;
	const auto tag  = (cs.cs_regs.data_addr * 0x9e3779b97f4a7c15ull) | 1ull;
	const auto slot = (tag >> 40u) & (m_table_dispatched.size() - 1);
	if (m_table_dispatched[slot].load(std::memory_order_relaxed) != tag) return 0;
	return m_table_dispatch_mixed_until[slot].load(std::memory_order_relaxed) < m_context.FrameNumber() ? 1u : 3u;
}
// (A compute stage whose dispatch took the normal path after one the table served keeps its resolutions ahead for
// TableMixedDispatchFrames: Demon's Souls (PPSA01341) at Boletaria alternates ~15 indirect dispatches a frame between
// the two, whose blocks or sets the table refuses now and then, and each one of those without its resolution chose its
// program on the execution thread, ~220 us.)
void RenderExecutor::TableNoteDispatched(const HW::ComputeShaderInfo& cs, bool tabled) noexcept {
	static constexpr uint64_t TableMixedDispatchFrames = 120;
	const auto tag  = (cs.cs_regs.data_addr * 0x9e3779b97f4a7c15ull) | 1ull;
	const auto slot = (tag >> 40u) & (m_table_dispatched.size() - 1);
	const bool was  = m_table_dispatched[slot].load(std::memory_order_relaxed) == tag;
	if (tabled && !was) m_table_dispatched[slot].store(tag, std::memory_order_relaxed);
	if (!tabled && was)
		m_table_dispatch_mixed_until[slot].store(m_context.FrameNumber() + TableMixedDispatchFrames, std::memory_order_relaxed);
}

bool RenderExecutor::TableLikely(const HW::Shader& shaders) const noexcept {
	if (TableXprMode() == 0) return false;
	const auto tag  = TableLikelyTag(shaders);
	const auto slot = (tag >> 40u) & (m_table_drawn.size() - 1);
	return m_table_drawn[slot].load(std::memory_order_relaxed) == tag &&
	       m_table_mixed_until[slot].load(std::memory_order_relaxed) < m_context.FrameNumber();
}

bool RenderExecutor::TableXprTry(CommandBuffer& buffer, uint64_t args_addr, uint32_t count, uint32_t stride,
                                 uint64_t index_base, uint64_t index_bytes, vk::IndexType index_type,
                                 const DrawIndexArgs* direct, bool clean, const ShaderIndirectOffsets& offsets,
                                 const uint32_t* run_offsets) {
	bool drawn = false;
	// (A pair whose try was refused after one that drew keeps its resolutions ahead for TableMixedFrames.)
	static constexpr uint64_t TableMixedFrames = 120;
	struct DrawnHint {
		std::atomic<uint64_t>& slot;
		std::atomic<uint64_t>& mixed;
		const uint64_t         tag;
		const uint64_t         frame;
		const bool&            drawn;
		~DrawnHint() {
			const bool was = slot.load(std::memory_order_relaxed) == tag;
			if (was && !drawn) mixed.store(frame + TableMixedFrames, std::memory_order_relaxed);
			if (!was && drawn) slot.store(tag, std::memory_order_relaxed);
		}
	};
	const auto      likely_tag  = TableLikelyTag(buffer.GetShaders());
	const auto      likely_slot = (likely_tag >> 40u) & (m_table_drawn.size() - 1);
	const DrawnHint hint {m_table_drawn[likely_slot], m_table_mixed_until[likely_slot], likely_tag, m_context.FrameNumber(), drawn};
	struct DrewDraws {
		std::atomic<bool>& flag;
		const bool&        drawn;
		~DrewDraws() {
			if (drawn && !flag.load(std::memory_order_relaxed)) flag.store(true, std::memory_order_relaxed);
		}
	} const drew_draws {m_table_drew_draws, drawn};
	if (m_table_store) Spec::Refuse("store: lost (the draw never reached TableStore)");
	m_table_store = false;
	// The user SGPRs the CP patches with the records' vertex and instance offsets (his START_INST_LOC): the draw's
	// vertex program is translated for them (the pair's key).
	g_table_offsets_key = direct != nullptr || (offsets.vertex == ShaderIndirectOffsets::None &&
	                                            offsets.instance == ShaderIndirectOffsets::None)
	                          ? -1
	                          : static_cast<int32_t>(uint32_t {offsets.instance} << 8u | offsets.vertex);
	const auto mode = TableXprMode();
	if (mode == 0 || buffer.IsInvalid()) return false;
	// What a table program cannot draw, known before the try: stages other than a vertex (ES) and a pixel program
	// (TableTry refuses them), a pair refused for good under this shader map generation. (A clean draw continues a
	// table draw's run, whose registers these are.) Each try costs its pending operations, the context lock, the
	// native XPR state and its key's hash: GTA V makes ~5,250 indexed draws a frame, none by table.
	const auto& try_vs = buffer.GetShaders().GetVs();
	if (try_vs.es_regs.data_addr == 0 || try_vs.ls_regs.data_addr != 0 || try_vs.hs_regs.data_addr != 0) return false;
	const auto refused_tag =
	    (likely_tag ^ (uint64_t {static_cast<uint32_t>(g_table_offsets_key)} * 0xff51afd7ed558ccdull)) | 1ull;
	const auto refused_slot       = (refused_tag >> 40u) & (m_table_refused.size() - 1);
	const auto refused_generation = ShaderMapGeneration();
	if (m_table_refused[refused_slot] == refused_tag && m_table_refused_generation[refused_slot] == refused_generation)
		return false;
	NativeXprDirectDraw direct_draw;
	if (direct != nullptr) {
		// A direct indexed draw of one object, as NativeXprTryDirect takes them.
		if (direct->indirect.address != 0 || direct->offset_source != DrawOffsetSource::DrawState ||
		    direct->index_addr == nullptr || direct->index_count == 0 || direct->instance_count == 0 ||
		    direct->first_instance != 0 || direct->index_type_and_size > 1)
			return false;
		index_base  = reinterpret_cast<uint64_t>(direct->index_addr);
		index_bytes = uint64_t {direct->index_count} * (direct->index_type_and_size == 0 ? 2u : 4u);
		index_type  = direct->index_type_and_size == 0 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
		direct_draw = {.index_address  = index_base,
		               .index_count    = direct->index_count,
		               .instance_count = direct->instance_count,
		               .base_vertex    = direct->base_vertex,
		               .indexed        = true,
		               .indirect       = false};
	}
	if (index_base == 0 || index_bytes == 0) return false;
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());
	(void)NativeXprState();
	NativeXprCollect();
	std::array<uint64_t, 64> run;
	if (direct == nullptr) {
		count = std::min<uint32_t>(std::max<uint32_t>(count, 1u), 64u);
		// (A run of packets: each record at its packet's data offset, GpuDrawIndirectArgs::run_offsets.)
		for (uint32_t i = 0; i < count; ++i)
			run[i] = run_offsets != nullptr ? args_addr - run_offsets[0] + run_offsets[5 * i] : args_addr + uint64_t {i} * stride;
	}
	const std::span<const uint64_t> commands = direct == nullptr ? std::span<const uint64_t> {run.data(), count}
	                                                              : std::span<const uint64_t> {};
	const auto* direct_ptr = direct == nullptr ? nullptr : &direct_draw;
	TableResult result     = TableResult::Native;
	Spec::Refuse("xpr: draw runs tried");
	if (clean) Spec::Refuse("xpr: clean draws");
	if (mode >= 2 && clean && TableContinue(buffer, commands, index_base, index_bytes, index_type, direct_ptr)) {
		result = TableResult::Drawn;
	} else {
		// What a table variant draws, as a native XPR record: an indexed triangle list of a vertex and a pixel stage
		// (no tessellation: TableTry; TableStore refuses mesh shading), with no draw data pushed (object id) and no
		// metadata color operation (CB_COLOR_CONTROL mode: fast clear eliminate, resolve...).
		const auto& ucfg = buffer.GetUserConfig();
		const auto& ctx  = buffer.GetRegisters();
		if (ucfg.GetPrimType() == Prospero::PrimitiveType::kTriList && !ctx.HasObjectPrimitiveIdOutput() &&
		    ctx.GetColorControl().mode <= 1u && (ucfg.GetPrimitiveResetControl() & 0x1u) == 0) {
			// The draw state's key, as his NativeXprTry computes it before its table try (the variants, the store and
			// the captured draw state go by it).
			m_native_xpr->state_key = NativeXprStateKeyOf(buffer, &m_state_key_cache, m_context_id, m_user_config_id);
			m_table_try_refused     = false;
			result = TableTry(buffer, commands, index_base, index_bytes, index_type, direct_ptr);
			if (result == TableResult::Native && std::exchange(m_table_try_refused, false)) {
				m_table_refused[refused_slot]            = refused_tag;
				m_table_refused_generation[refused_slot] = refused_generation;
			}
		} else {
			Spec::Refuse("xpr: not a plain triangle list");
		}
	}
	switch (result) {
		case TableResult::Drawn: drawn = true; return true;
		case TableResult::Store: m_table_store = true; break;
		case TableResult::Deferred: break;
		case TableResult::Native: break;
	}
	return false;
}

#if KYTY_CX_DISPATCH
// KYTY_TABLE_DISPATCH=1 (on by default in Demon's Souls; TableLoadSwitches): a compute shader the normal path dispatched
// (its resource tables evaluated in full), whose program allows table mode, is dispatched as table draws are drawn: its
// slots in a block, its read-only V#s resolved once, its written ones obtained as written at every dispatch (the buffer
// cache tracks the GPU's writes), its images and samplers in a set, the barriers as the normal path's. Direct
// dispatches of thread groups or of thread counts, and indirect dispatches of thread groups (their arguments read by
// the GPU); indirect dispatches of thread counts (the normal path reads the counts) and the shaders a special path of
// the normal path took (metadata clears, fills) stay on the normal path. (chenxiao07's TableDispatch, table-xpr.inc.)

// The compute stage's registers that select the program (ShaderGetStaticInputInfoCS reads them), and whether the
// dispatch counts threads (another program: ShaderComputeInputInfo::dispatch_thread_dimensions).
static std::array<uint64_t, 4> TableDispatchWords(const HW::ComputeShaderInfo& cs, bool thread_dimensions) {
	const auto& r = cs.cs_regs;
	return {r.data_addr, uint64_t {r.num_thread_x} | uint64_t {r.num_thread_y} << 32u,
	        uint64_t {r.num_thread_z} | uint64_t {r.lds_size} << 32u,
	        uint64_t {r.wave_size} | uint64_t {r.user_sgpr} << 8u | uint64_t {r.tidig_comp_cnt} << 16u |
	            uint64_t {r.tgid_x_en} << 24u | uint64_t {r.tgid_y_en} << 25u | uint64_t {r.tgid_z_en} << 26u |
	            uint64_t {r.tg_size_en} << 27u | uint64_t {r.scratch_en} << 28u | uint64_t {thread_dimensions} << 29u};
}
static uint64_t TableDispatchKey(const std::array<uint64_t, 4>& words) {
	return XXH3_64bits(words.data(), sizeof(words));
}
static uint64_t TableDispatchKey(const HW::ComputeShaderInfo& cs, bool thread_dimensions) {
	return TableDispatchKey(TableDispatchWords(cs, thread_dimensions));
}

// What table dispatches serve: a table program whose shader data fits the push constants, without GDS or LDS kept in a
// device buffer (the table set binds neither), and the thread counts pushed only for a dispatch of thread counts.
static bool TableDispatchServes(const ShaderRecompiler::IR::CompiledShaderInfo& program, bool thread_dimensions) {
	using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
	return program.table_mode && program.bindings.UsesPushData() && !program.info.uses_gds &&
	       program.bindings.descriptor_counts[static_cast<size_t>(Kind::SharedMemory)] == 0 &&
	       (thread_dimensions || program.bindings.dispatch_thread_dword == ShaderRecompiler::IR::PushData::NoStart);
}

// The normal path took a dispatch of the compute stage's registers by one of its special paths (`why`: a metadata clear,
// a fill): the shader stays there for good, as his TableDispatchSeen with a special path marks it.
void RenderExecutor::TableDispatchSpecial(const HW::ComputeShaderInfo& cs, bool thread_dimensions, const char* why) {
	if (TableDispatchMode() == 0 || TableXprMode() == 0) return;
	auto&      table = OwnTable();
	const auto key   = TableDispatchKey(cs, thread_dimensions);
	auto       it    = table.dispatches.find(key);
	if (it != table.dispatches.end() && it->second.refused) return;
	if (it == table.dispatches.end()) {
		it            = table.dispatches.try_emplace(key).first;
		it->second.vs = cs.cs_regs.data_addr;
	}
	auto& pair = it->second;
	if (pair.programs[0] != nullptr) {
		const auto write = CatalogWrite();
		TableReset(pair);
	}
	pair.refused = true;
	pair.special = why;
}

// The normal path dispatched `module` (any but an indirect dispatch of thread counts): the shader pair of its registers
// learns the table program for that dispatch's specialization (it compiles on the shader workers meanwhile), and its
// pipeline. (His TableDispatchSeen counts the sightings and asks for the program of the compute stage's registers
// itself; the normal path's lookup is what chooses the specialization here.) Programs table mode cannot serve
// (indirect descriptors and images, GDS, ...) take the normal path for good.
void RenderExecutor::TableDispatchLearn(const HW::ComputeShaderInfo& cs, const ShaderModule& module, bool async_compute,
                                        bool thread_dimensions) {
	if (TableDispatchMode() == 0 || TableXprMode() == 0) return;
	auto&      table = OwnTable();
	const auto key   = TableDispatchKey(cs, thread_dimensions);
	auto       it    = table.dispatches.find(key);
	if (it == table.dispatches.end()) {
		it     = table.dispatches.try_emplace(key).first;
		it->second.vs = cs.cs_regs.data_addr;
	}
	auto& pair = it->second;
	++pair.sightings;
	pair.used_frame = m_context.FrameNumber();
	if (pair.refused || pair.programs[0] != nullptr || pair.vs != cs.cs_regs.data_addr)
		return void();
	if (pair.waiting) {
		if (!pair.waiting->load(std::memory_order_acquire)) return;
		pair.waiting.reset();
	}
	const auto& program = module.info;
	auto&       pipelines = m_context.GetPipelineCache();
	const auto [plan, specialization] = pipelines.ProgramPlanOf(module);
	const auto refuse = [&](const char* why) {
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
			std::printf("Table: CS 0x%llx refused: %s\n", static_cast<unsigned long long>(pair.vs), why);
		pair.refused = true;
		pair.special = why;
	};
	if (plan == nullptr || specialization == nullptr) return refuse("no resource plan");
	if (program.info.uses_gds ||
	    (!thread_dimensions && program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) ||
	    program.bindings.ray_query_dword != ShaderRecompiler::IR::PushData::NoStart ||
	    program.info.uses_image_heap || module.guest_traversal || program.info.bvh ||
	    !program.info.self_sized_buffers.empty() ||
	    plan->uniform_fill.fill.kind != ShaderRecompiler::IR::UniformFillKind::None ||
	    std::ranges::any_of(program.info.buffers, [](const auto& buffer) { return buffer.indirect_write_table; }))
		return refuse("a program table mode does not serve");
	if (RayTracing::TlasSource source; RayTracing::FindTlasSource(program.info, source))
		return refuse("native ray tracing");
	auto compute = std::make_unique<ShaderComputeInputInfo>();
	compute->async_compute              = async_compute;
	compute->dispatch_thread_dimensions = thread_dimensions;
	// (The buffers the pair saw change format are specialized for none, as a draw's: TableStore.)
	auto portable = *specialization;
	for (size_t i = 0; i < portable.buffers.size() && i < 32; ++i) {
		if (((pair.portable[0] >> i) & 1u) == 0) continue;
		portable.buffers[i].descriptor_format  = Prospero::BufferFormat::kInvalid;
		portable.buffers[i].descriptor_swizzle = DstSel(4, 5, 6, 7);
	}
	const auto result = pipelines.GetComputeProgramTable(cs, *compute, portable);
	if (result.status == PipelineCache::TableStatus::Pending) return void();
	if (result.status == PipelineCache::TableStatus::Refused || result.module == nullptr ||
	    !TableDispatchServes(result.module->info, thread_dimensions))
		return refuse(result.refusal != nullptr ? result.refusal->c_str() : "table mode cannot serve it");
	compute->stage = {.module = result.module, .resources = &g_table_no_resources};
	// (Made by a background thread, as a draw pair's: creating it here stalled the execution thread as areas came into
	// view. A later sighting takes it.)
	auto* const pipeline = pipelines.TryGetComputePipeline(*compute);
	if (pipeline == nullptr) return void();
	if (!pipeline->native_bindings || pipeline->uses_push_descriptors) return refuse("pipeline");
	pair.compute     = std::move(compute);
	pair.tables[0]   = result.module;
	pair.programs[0] = &result.module->info;
	pair.plans[0]    = plan;
	pair.code_hashes[0] = ShaderHashAt(pair.vs);
	pair.pipeline    = pipeline;
	TableRuns(pair);
#if KYTY_CX_SCOUT
	TableScoutPublish(pair); // (the resolve thread evaluates this pair's dispatches ahead: KYTY_SCOUT=3)
#endif
}

// A dispatch from the shader pair's table program (see TableDispatchLearn): the slots and buffers of the compute stage's
// plan evaluated into a block, the images and samplers in a descriptor set (TableSet), the block's address pushed with
// the user data. `indirect_args`: the guest address of the GPU-read thread group counts (an indirect dispatch), else
// `groups_*` are the groups, or the threads with `thread_dimensions`. False: the normal path dispatches.
bool RenderExecutor::TableDispatch(CommandBuffer& buffer, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z,
                                   uint64_t indirect_args, bool thread_dimensions) {
	if (TableDispatchMode() == 0) return false;
	// (An indirect dispatch of thread counts: the normal path reads the counts the GPU wrote.)
	if (indirect_args != 0 && thread_dimensions) return false;
	auto& table = OwnTable();
	if (table.dispatches.empty() || buffer.IsInvalid()) return false;
	TableSweepPairs();
	const auto& sh    = buffer.GetShaders();
	const auto& cs    = sh.GetCs();
	const auto  words = TableDispatchWords(cs, thread_dimensions);
	// (The pair found last again without its hash or lookup: runs of dispatches keep their program. A pointer stays
	// valid until TableSweepPairs, which drops this one.)
	auto* known = table.found_dispatch_key == words ? table.found_dispatch : nullptr;
	if (known == nullptr) {
		const auto found = table.dispatches.find(TableDispatchKey(words));
		if (found == table.dispatches.end()) return false;
		known                    = &found->second;
		table.found_dispatch_key = words;
		table.found_dispatch     = known;
	}
	auto& pair = *known;
	if (pair.refused || pair.programs[0] == nullptr || pair.pipeline == nullptr || pair.vs != cs.cs_regs.data_addr)
		return false;
	TablePrefetchPair(pair);
	pair.used_frame = m_context.FrameNumber();
	TablePrefetchNext(table.previous_dispatch, pair);
	LiveCensus::Scope census(LiveCensus::DrawPhase, 0x7ab1f, 0); // census: the dispatches' phases (b)
	// The sets of the dispatches retire here too (TableXprTry collects them for the draws).
	NativeXprCollect();
	if (const auto generation = ShaderMapGeneration(); generation != pair.Generation(Role())) {
		if (ShaderHashAt(pair.vs) != pair.programs[0]->shader_hash) {
			const auto write = CatalogWrite();
			pair.portable    = {};
			TableReset(pair);
			return false;
		}
		pair.Generation(Role()) = generation;
	}
	// A buffer whose V#s changed format (TableBlocks): the program is learned again with its format decoded at run time
	// (TableDispatchLearn, from the normal path's next dispatch of the pair).
	if (pair.respecialize) {
		const auto write = CatalogWrite();
		TableReset(pair);
		return false;
	}
	TablePrefetchArrays(pair, pair.LastSet(Role()));
	const std::array<std::span<const uint32_t>, 2> user_data {
	    std::span<const uint32_t>(cs.cs_user_sgpr.value, std::min<uint32_t>(cs.cs_regs.user_sgpr, HW::UserSgprInfo::SGPRS_MAX)),
	    std::span<const uint32_t> {}};
	std::optional<LiveCensus::Scope> phase;
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1f, 1);
	// KYTY_SCOUT=3: the evaluation the resolve thread made for this dispatch's registers, when its ticket holds (as a
	// table draw's).
#if KYTY_CX_SCOUT
	const auto* scout = Scout::Mode() == 3 && Scout::Running() && m_table_ticket != nullptr
	                        ? TableScoutTicket(pair, user_data, 1, m_context, static_cast<const Scout::Ticket*>(m_table_ticket))
	                        : nullptr;
	if (scout != nullptr) {
		TableScoutApply(table, pair, *scout, 1);
	} else
#endif
	{
		if (!TableEvaluate(pair, user_data)) return false;
		TableGather(pair, user_data);
	}
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1f, 2);
	auto* set = TableSet(buffer, pair, nullptr, nullptr);
	if (set == nullptr) return false;
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1f, 3);
	std::array<vk::DeviceAddress, 2> blocks {};
	bool                             storage_writes = false;
	auto&                            cache          = m_context.GetBufferCache();
	TablePrepareBda(pair, user_data); // (as TableDraw's: before the blocks)
	const auto registration = cache.RegistrationEpoch(); // (as TableDraw's)
	if (!TableBlocks(pair, user_data, blocks, &storage_writes)) return false;
	// An indirect dispatch's arguments, as the normal path's DispatchIndirect obtains them.
	Buffer*  args_owner  = nullptr;
	uint64_t args_offset = 0;
	if (indirect_args != 0) {
		std::tie(args_owner, args_offset) = cache.ObtainBuffer(indirect_args, sizeof(vk::DispatchIndirectCommand), false);
		if (args_owner == nullptr || (args_offset & 3u) != 0) return false;
	}
	if (cache.RegistrationEpoch() != registration) return LiveCounters::Add(LiveCounters::TableRegistered), false;
	phase.reset();
	phase.emplace(LiveCensus::DrawPhase, 0x7ab1f, 4);
	// Emission, as DispatchDirect's and DispatchIndirect's.
	buffer.EndRendering();
	auto       vk_buffer         = buffer.Handle();
	auto&      dispatch_textures = m_context.GetTextureCache();
	const auto images_epoch      = dispatch_textures.ImagesEpoch();
	// (As a draw's: the set's images as its last dispatch's transit left them, while no image changed since.)
	if ((set->transit_epoch != images_epoch || set->transit_state != nullptr) && !TableTransit(*set, vk_buffer, nullptr))
		return false;
	set->transit_epoch = dispatch_textures.ImagesEpoch();
	set->transit_state = nullptr;
	const auto& pipeline = *pair.pipeline;
	const auto& program  = *pair.programs[0];
	const auto& bindings = program.bindings;
	vk_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.pipeline_layout, 0, 1, &set->set, 0, nullptr);
	ShaderRecompiler::IR::PushData push_data;
	for (size_t i = 0; i < program.info.user_data_registers.size(); ++i) {
		const auto index = program.info.user_data_registers[i] - program.user_data_base;
		push_data.dwords[bindings.push_data_start_dword + i] = index < user_data[0].size() ? user_data[0][index] : 0;
	}
	// (A dispatch of thread counts: the counts, which the program bounds its threads by, as the normal path pushes them.)
	if (thread_dimensions && bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		push_data.dwords[bindings.push_data_start_dword + bindings.dispatch_thread_dword]      = groups_x;
		push_data.dwords[bindings.push_data_start_dword + bindings.dispatch_thread_dword + 1u] = groups_y;
		push_data.dwords[bindings.push_data_start_dword + bindings.dispatch_thread_dword + 2u] = groups_z;
	}
	push_data.dwords[bindings.push_data_start_dword + bindings.TableBlockDword()]     = static_cast<uint32_t>(blocks[0]);
	push_data.dwords[bindings.push_data_start_dword + bindings.TableBlockDword() + 1] = static_cast<uint32_t>(blocks[0] >> 32u);
	vk_buffer.pushConstants(pipeline.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                        (bindings.push_data_start_dword + bindings.ShaderDataDwords()) * sizeof(uint32_t),
	                        push_data.dwords.data());
	// A dispatch the guest did not order after the previous one of its queue overlaps it (DispatchDirect).
	const bool chained = std::exchange(m_dispatch_chained, false) && m_deferred_barrier;
	if (storage_writes && !chained) ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	if (args_owner != nullptr) {
		// (As DispatchIndirect: chained, the barrier before the previous dispatch ordered the arguments' writes.)
		if (!chained) {
			vk::MemoryBarrier barrier {};
			barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
			barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
			vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
			                              vk::PipelineStageFlagBits::eTransfer,
			                          vk::PipelineStageFlagBits::eDrawIndirect, {}, 1, &barrier, 0, nullptr, 0, nullptr);
		}
		vk_buffer.dispatchIndirect(args_owner->Handle(), args_offset);
	} else if (thread_dimensions) {
		// Thread counts: the groups that cover them, as the normal path's.
		const auto& r      = cs.cs_regs;
		const auto  groups = [](uint32_t threads, uint32_t size) { return (threads + std::max(size, 1u) - 1u) / std::max(size, 1u); };
		vk_buffer.dispatch(groups(groups_x, r.num_thread_x), groups(groups_y, r.num_thread_y), groups(groups_z, r.num_thread_z));
	} else {
		vk_buffer.dispatch(groups_x, groups_y, groups_z);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	m_deferred_barrier       = true;
	m_deferred_barrier_queue = m_guest_queue;
	// (A later table draw keeps its pipeline, set and state: the dispatch binds none of the graphics bind point, and an
	// image its set moved moves TextureCache::ImagesEpoch.)
	LiveCounters::Add(LiveCounters::TableDispatches);
	return true;
}
#endif // KYTY_CX_DISPATCH
