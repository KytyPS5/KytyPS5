// --- Native XPR draws ------------------------------------------------------------------------
//
// Included by renderDraw.cpp. Ported from chenxiao07's KytyPS5 fork (src/local/native-xpr.inc)
// onto this renderer, as a layer the normal draw path backs: a draw the layer cannot prove takes
// the normal path, which stores a record of it when asked.
//
// An indexed indirect draw whose arguments the host GPU reads (TryExecuteDrawIndirectOnGpu) is
// offered to NativeXprTry() first. Two entries make it native:
//  * a draw state, keyed by every graphics register except user data: the render targets with
//    their attachment layouts, and the dynamic state;
//  * an object record, keyed by the shader addresses and the user data: one descriptor set
//    written once with the object's buffers, images and samplers; the stage constants (flattened
//    SRT and shader data, which go into the stream buffer at each draw, at the set's dynamic
//    offsets); per draw state, the pipeline variant with native bindings (the same SPIR-V); and
//    what proves the record current.
// A record is proven current at every use: every guest word the SRT evaluation read still has
// the logged value, read as the resolution's read log reads it (GuestReadLog); words learned to
// land only in the flattened SRT (from an earlier store of the same key) are patched. Once per
// frame, or when the buffer or image set moved: every buffer is still the same live buffer and
// CPU clean, every image the same registered object, not CPU dirty. Anything else falls back to
// the normal path, which stores again.
//
// Relocated records serve objects whose user data move between frames (GTA V's SRT pointers
// point into per-frame memory, so no key comes again): when the key misses, the draw's resolution
// ahead (the resolve thread evaluated its programs; the guest words it read still hold) is looked
// up by its programs and descriptors. A record with the same programs, images, samplers and buffer
// descriptors takes the fresh flattened SRT and user data; one whose buffer bases moved is rebound.
//
// Off by default (KYTY_NATIVE_XPR=1 turns the records on); KYTY_NATIVE_XPR_STATS=1 prints counters every 300 frames.

namespace {

// The records are the fallback of the table draws (tableXpr.inc) in chenxiao07's tree. Off unless KYTY_NATIVE_XPR=1: this
// port of them draws Demon's Souls' walls dark, and in GTA V they cost more than they save (55.8 -> 53.8 fps in
// Performance mode).
bool NativeXprEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_NATIVE_XPR");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// KYTY_NATIVE_XPR_RELOCATE=0: no relocated records (see the top of this file). On by default, as chenxiao07's
// Demon's Souls launcher sets it (native XPR draws are Demon's Souls' only; in GTA V nearly every use rebinds moved
// buffers, a new set, and its normal path is cheaper).
bool NativeXprRelocateEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_NATIVE_XPR_RELOCATE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// chenxiao07's KYTY_NATIVE_XPR_INSTANCES (=1; off by default here): the instances of a mesh (the same programs and
// descriptor shape, other buffers) keep a relocated record each whatever order a frame draws them in, instead of
// rebinding one record's buffers (a new set) for each. With it Demon's Souls crashed in the driver on entering the
// game (2 of 2 runs; an access violation at a bad address): not understood yet.
bool NativeXprInstancesEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_NATIVE_XPR_INSTANCES");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// KYTY_NATIVE_XPR_DIRECT=0: direct indexed draws take the normal path; KYTY_NATIVE_XPR_VB=0: draws
// with vertex buffers are not stored.
bool NativeXprSwitch(const char* name) {
	const char* value = std::getenv(name);
	return value == nullptr || value[0] != '0';
}

bool NativeXprDirectEnabled() {
	static const bool enabled = NativeXprSwitch("KYTY_NATIVE_XPR_DIRECT");
	return enabled;
}

bool NativeXprVertexBuffersEnabled() {
	static const bool enabled = NativeXprSwitch("KYTY_NATIVE_XPR_VB");
	return enabled;
}

// KYTY_NATIVE_IMAGE_PROOF=0: a record's sampled images are checked again whenever any image was registered or
// unregistered, instead of only those whose own registration changed.
bool NativeXprImageProofEnabled() {
	static const bool enabled = NativeXprSwitch("KYTY_NATIVE_IMAGE_PROOF");
	return enabled;
}

// chenxiao07's KYTY_ASYNC_XPR_PIPELINES (default on; 0 creates them on the execution thread): a record's pipeline
// variant is created on a background thread; its store waits for it (the draws take the normal path meanwhile).
bool NativeXprAsyncPipelines() {
	static const bool enabled = NativeXprSwitch("KYTY_ASYNC_XPR_PIPELINES");
	return enabled;
}

uint32_t NativeXprNumberSwitch(const char* name, uint32_t default_value) {
	const char* value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : default_value;
}

// chenxiao07's KYTY_NATIVE_XPR_STORE_BUDGET (default 256; 0: no limit): record stores asked for per frame. Walking
// brings a thousand or more new objects a frame, and storing them all at once made 60-90 ms frames.
uint32_t NativeXprStoreBudget() {
	static const uint32_t budget = NativeXprNumberSwitch("KYTY_NATIVE_XPR_STORE_BUDGET", 256u);
	return budget;
}

// chenxiao07's KYTY_NATIVE_XPR_KEEP_FRAMES (default 600, at least 2): the frames an unused record stays; past
// NativeXprCache::RecordsSoftCap records, 2.
uint64_t NativeXprKeepFrames() {
	static const uint64_t keep = std::max<uint64_t>(2u, NativeXprNumberSwitch("KYTY_NATIVE_XPR_KEEP_FRAMES", 600u));
	return keep;
}

// KYTY_NATIVE_XPR_PREDICT=0: no prefetch of the record the next draw used last time.
bool NativeXprPredictEnabled() {
	static const bool enabled = NativeXprSwitch("KYTY_NATIVE_XPR_PREDICT");
	return enabled;
}

bool NativeXprStatsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_NATIVE_XPR_STATS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// A word target in a buffer descriptor's base address (dword 0, or the low half of dword 1).
int32_t NativeXprBufferPatch(uint32_t stage, uint32_t resource, uint32_t dword) {
	return -16 - static_cast<int32_t>(stage << 20u | resource << 4u | dword);
}

struct NativeXprKeyHash {
	[[nodiscard]] size_t operator()(uint64_t key) const noexcept { return static_cast<size_t>(key); }
};

uint64_t NativeXprHash(const std::vector<uint32_t>& words) {
	return XXH3_64bits(words.data(), words.size() * sizeof(uint32_t));
}

// T# fields that only feed the LOD statistics (MIN_LOD_WARN, MIP_STATS_CNT_EN, MIP_STATS_CNT_ID):
// Demon's Souls reassigns the counters every frame. They select no image and no view.
constexpr std::array<uint32_t, 8> NativeXprLodStatsMask {0, 0, 0, 0, 0, (0xfffu << 8u) | (1u << 25u), 0xffu, 0};

bool NativeXprSameImagesButLodStats(const std::vector<ShaderRecompiler::IR::DescriptorValue>& a,
                                    const std::vector<ShaderRecompiler::IR::DescriptorValue>& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].dword_count != b[i].dword_count) return false;
		for (uint32_t w = 0; w < a[i].dwords.size(); ++w)
			if ((a[i].dwords[w] & ~NativeXprLodStatsMask[w]) != (b[i].dwords[w] & ~NativeXprLodStatsMask[w]))
				return false;
	}
	return true;
}

} // namespace

// The render targets as a draw under one state left them, and its dynamic state.
struct RenderExecutor::NativeXprDrawState {
	struct Target {
		ImageId               id;
		uint64_t              serial   = 0;
		vk::ImageLayout       layout   = vk::ImageLayout::eUndefined;
		vk::AccessFlags2      access;
		ImageSubresourceRange range;
		uint64_t              metadata = 0; // surface metadata (HTile/DCC) address, 0 none
		uint32_t              slice    = 0;
		ImageInfo             requested; // what the registers ask for (TextureCache::IsTargetCurrent)
		ImageViewInfo         view;      // and the attachment's view (a color target's fast clear: NativeXprEmitTargets)
		mutable uint64_t      clear_memo = 0; // TextureCache::ColorClearSettled's memo
	};
	RenderState         rendering; // without clears: a reopened rendering loads
	std::vector<Target> targets;
	DynamicState        dynamic;
	// ImageSetEpoch under which the targets were what a lookup finds.
	mutable uint64_t    targets_epoch = 0;
	mutable uint64_t    used_frame    = 0;
	// TextureCache::ImagesEpoch after a table draw last emitted the targets (NativeXprEmitTargets): they stay in place
	// while it does not move (UINT64_MAX: not yet).
	mutable uint64_t    emitted_epoch = UINT64_MAX;
	// The sum of its targets' image serials then (KYTY_TABLE_SERIALS; UINT64_MAX: none).
	mutable uint64_t    emitted_serials = UINT64_MAX;
	// Times the state was captured at this address (NativeXprCaptureState): what was proven under an earlier capture
	// (an image set's transit) holds no more. `capture_id`: unique per capture.
	uint64_t            captures   = 0;
	uint64_t            capture_id = 0;
	// The dynamic state's hash (two states compare it first) and the targets' ids alone (the serial sums read them).
	uint64_t             dynamic_hash = 0;
	std::vector<ImageId> target_ids;
	void                   Use(uint64_t frame) const { used_frame = frame; }
	[[nodiscard]] uint64_t UsedFrame() const { return used_frame; }
};

// One object: its resources under the compiled programs it was translated with.
struct RenderExecutor::NativeXprRecord {
	struct Stage {
		const ShaderRecompiler::IR::CompiledShaderInfo* program = nullptr;
		std::vector<uint32_t> shader_data; // push data and/or the shader-data binding
		bool     srt_binding  = false;
		bool     data_binding = false;
		uint32_t push_start   = ShaderRecompiler::IR::PushData::NoStart;
	};
	struct BufferDep {
		uint64_t   address = 0, size = 0;
		BufferId   id;
		vk::Buffer handle  = nullptr;
		bool       texel   = false;
		uint32_t   stage = 0, resource = 0, slot = 0; // program.info.buffers[resource], descriptor slot
	};
	struct ImageDep {
		ImageId                 id;
		uint64_t                serial = 0;
		uint32_t                registration = 0; // Image::registrations when the record was proven
		vk::ImageLayout         layout = vk::ImageLayout::eUndefined;
		ImageSubresourceRange   range;
		TextureCache::ImageDesc desc;
	};
	// A dynamic storage buffer of the set: its binding number and what it holds.
	struct DynamicBinding {
		uint32_t binding = 0;
		uint32_t stage   = 0;
		bool     srt     = false;
	};
	std::vector<uint32_t>       key;
	std::array<Stage, 2>        stages;
	// The resources of each stage's SRT evaluation, as the record's own copy: the guest rewrites constants and
	// descriptor words after it submits its draws, and the read log refreshes them in place at every use (as it
	// does for a draw's resolution ahead).
	std::array<ShaderRecompiler::IR::ResourceSnapshot, 2>       snapshots;
	std::array<const ShaderRecompiler::IR::ResourcePlan*, 2> plans {};
	uint32_t                    stage_count = 1;
	uint64_t                    vertex_program = 0, pixel_program = 0;
	vk::DescriptorSet           set  = nullptr;
	vk::DescriptorPool          pool = nullptr;
	vk::Buffer                  upload = nullptr; // the stream buffer the dynamic bindings point into
	std::vector<DynamicBinding> dynamic_bindings;  // by binding number
	vk::ShaderStageFlags        push_stages;
	bool                        has_push = false;
	// Native pipeline variant per draw state this object was drawn under.
	std::vector<std::pair<uint64_t, const PipelineCache::Pipeline*>> pipelines;
	// Validation.
	GuestReadLog           reads;
	// The set's payload, to write a new set when a buffer base moved (NativeXprRebindBuffers).
	struct WriteSlot {
		uint32_t           binding = 0, count = 0, start = 0;
		vk::DescriptorType type {};
		bool               image = false;
	};
	std::vector<WriteSlot>                set_writes;
	std::vector<vk::DescriptorBufferInfo> set_buffers;
	std::vector<vk::DescriptorImageInfo>  set_images;
	std::array<uint32_t, 2>               buffers_start {UINT32_MAX, UINT32_MAX}; // per stage, in set_buffers
	std::vector<BufferDep> buffers;
	std::vector<ImageDep>  images;
	uint64_t mapping_version   = 0;
	uint64_t buffer_set_epoch  = 0;
	uint64_t image_set_epoch   = 0;
	uint64_t cpu_epoch         = 0; // sum of the buffer regions' CPU epochs, 0 unknown
	uint64_t image_dirty_epoch = 0; // Image::CpuDirtyEpoch() when the images were proven clean
	uint64_t validated_frame   = UINT64_MAX;
	uint64_t used_frame        = 0;
	uint64_t uses              = 0;
	bool     fetch_embedded = false; // the vertex program fetches (ResolveDrawOffsets())
	bool     vertex_buffers = false; // the draw binds vertex buffers: taken from its resolution
	// Relocated records (no read log): their hashes (NativeXprRelocationHashes).
	bool     relocated  = false;
	uint64_t full_hash  = 0;
	uint64_t shape_hash = 0;
	// The key hash of the record the next native draw used after this one, last time (KYTY_NATIVE_XPR_PREDICT).
	uint64_t next_hash = 0;
};

// Records are used about once a frame, so each is cold when its draw comes: the record the next draw used last
// time is pulled into the cache while this one is validated and emitted.
template <typename Record>
static void NativeXprPrefetch(const Record& record) {
	const auto prefetch = [](const void* address) {
		if (address != nullptr) _mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
	};
	prefetch(&record);
	prefetch(reinterpret_cast<const char*>(&record) + 64);
	prefetch(&record.cpu_epoch);
	prefetch(&record.used_frame);
	prefetch(record.key.data());
	prefetch(record.pipelines.data());
	prefetch(record.buffers.data());
	prefetch(record.images.data());
	prefetch(record.set_buffers.data());
}

struct RenderExecutor::NativeXprCache {
	vk::Device device;
	// Per object key, up to MaxVariants records, the last one used first: objects whose resources rotate
	// through a few values (a texture or buffer double-buffered per frame) keep a record per value.
	static constexpr size_t MaxVariants = 4;
	std::unordered_map<uint64_t, std::vector<std::unique_ptr<NativeXprRecord>>, NativeXprKeyHash> records;
	size_t record_count = 0;
	static constexpr size_t RecordsSoftCap = 49152;
	// Store requests of this frame (KYTY_NATIVE_XPR_STORE_BUDGET).
	uint64_t store_frame    = UINT64_MAX;
	uint32_t store_requests = 0;
	uint64_t last_hash = 0; // the key hash of the last native draw's record (KYTY_NATIVE_XPR_PREDICT)
	// Draw states stay at their address (the table draws' emission tracking compares pointers): a state captured
	// again is written over the old one (NativeXprCaptureState).
	std::unordered_map<uint64_t, std::unique_ptr<NativeXprDrawState>, NativeXprKeyHash> states;
	// The draw state the last table try found, and its key (TableTry).
	const NativeXprDrawState* last_state     = nullptr;
	uint64_t                  last_state_key = 0;
	// What the last table draw (tableDraw.inc) bound in the command buffer, while the scheduler's DynamicState
	// generation is the same (no other draw, helper pipeline or command buffer since): its draw state (dynamic state
	// and rendering), pipeline and index buffer, which the next table draw does not bind again.
	struct Emit {
		uint64_t                       generation = UINT64_MAX;
		const NativeXprDrawState*      state      = nullptr;
		const PipelineCache::Pipeline* pipeline   = nullptr;
		uint64_t                       index_base = 0, index_bytes = 0;
		vk::IndexType                  index_type = vk::IndexType::eUint16;
	} emit;
	struct RetiredSet {
		vk::DescriptorPool pool;
		vk::DescriptorSet  set;
		uint64_t           tick = 0;
	};
	std::vector<RetiredSet> retired;
	struct FreeSet {
		vk::DescriptorPool pool;
		vk::DescriptorSet  set;
	};
	static constexpr size_t FreeSetsPerLayout = 2048, FreeSetsMax = 16384;
	std::unordered_map<VkDescriptorSetLayout, std::vector<FreeSet>> free_sets;
	std::unordered_map<VkDescriptorSet, VkDescriptorSetLayout>      set_layouts;
	size_t                                                          free_count = 0;
	std::vector<vk::DescriptorPool>                                 pools;
	// Keys that missed, with the frame of their latest miss: a record pays off only when its key
	// comes again, so it is stored at the second sighting within two frames.
	struct SeenKey {
		uint64_t hash = 0, frame = 0;
	};
	std::vector<SeenKey> seen = std::vector<SeenKey>(8192);
	bool SeenAgain(uint64_t hash, uint64_t frame) { return SeenAgainIn(seen, hash, frame); }
	static bool SeenAgainIn(std::vector<SeenKey>& seen, uint64_t hash, uint64_t frame) {
		hash             = hash != 0 ? hash : 1;
		const auto mask  = seen.size() - 1;
		SeenKey*   empty = nullptr;
		for (uint64_t i = 0, at = hash & mask; i < 8; ++i, at = (at + 1) & mask) {
			auto& entry = seen[at];
			if (entry.hash == hash) {
				const bool again = entry.frame + 2 >= frame;
				entry.frame      = frame;
				return again;
			}
			if (empty == nullptr && (entry.hash == 0 || entry.frame + 2 < frame)) empty = &entry;
		}
		*(empty != nullptr ? empty : &seen[hash & mask]) = {hash, frame};
		return false;
	}
	// Keys whose store was refused: the same refusal would come every frame.
	static constexpr uint64_t RefuseFrames = 64;
	std::unordered_map<uint64_t, uint64_t, NativeXprKeyHash> refused;
	// Per shader-address pair: a pair whose programs cannot be recorded stops asking.
	std::unordered_map<uint64_t, bool, NativeXprKeyHash> unstorable;
	// Per pair: tries since its last record was stored; a pair whose objects never come again
	// (their user data move every frame) rests for a while.
	struct PairUse {
		uint64_t tries = 0, stored = 0, resting_until = 0;
	};
	static constexpr uint64_t PairTriesWithoutRecord = 2048, PairRestFrames = 1800;
	std::unordered_map<uint64_t, PairUse, NativeXprKeyHash> pairs;
	// Relocated records by the full hash of their programs and descriptors, and the latest record
	// of each shape (the same but the buffer bases).
	std::unordered_map<uint64_t, std::unique_ptr<NativeXprRecord>, NativeXprKeyHash> relocated;
	std::unordered_map<uint64_t, NativeXprRecord*, NativeXprKeyHash>                 relocated_shapes;
	std::vector<SeenKey> relocated_seen = std::vector<SeenKey>(8192);
	bool                 store_relocated = false; // the store asked for is a relocated one
	bool                 resolution_valid = false; // this try proved the draw's resolution current
	uint64_t             relocated_full = 0, relocated_shape = 0;
	// Scratch.
	std::vector<uint32_t>                      key;
	std::vector<std::pair<uint32_t, uint32_t>> changed;
	std::vector<std::pair<uint32_t, uint32_t>> rebind_buffers; // (stage, buffer resource)
	// The latest try: its keys, for the store the normal path makes when it asks for one.
	uint64_t tried_hash = 0;
	uint64_t state_key  = 0;
	// Resolution of a store's programs with a read log (see NativeXprStore()).
	std::unique_ptr<DrawRenderStorage, void (*)(DrawRenderStorage*)> scratch {nullptr, nullptr};
	ProgramResolution                                                 scratch_programs;
	uint64_t swept_frame    = UINT64_MAX;
	uint64_t collected_frame = UINT64_MAX;
	// Draw states leave when no draw used them for StateKeepFrames (NativeXprCollect: every StateSweepFrames, past
	// StatesKept): their keys hold the render targets' addresses, and entering an area made a few thousand that were
	// never used again.
	static constexpr uint64_t StateSweepFrames = 600, StateKeepFrames = 3600;
	static constexpr size_t   StatesKept       = 2048;
	uint64_t                  states_swept_frame = UINT64_MAX;
	uint64_t reported_frame = 0;
	struct Counters {
		uint64_t tries = 0, hits = 0, miss_key = 0, miss_state = 0, miss_target = 0, miss_validate = 0,
		         miss_emit = 0, stores = 0, stored = 0, refused = 0;
		uint64_t validate_unmapped = 0, validate_read = 0, validate_unlearned = 0, validate_resources = 0;
		// Learning: 0 learned, 1 reads, 2 programs, 3 buffers, 4 images, 5 samplers, 6 other structure, 7 unexplained.
		std::array<uint64_t, 8> learn {};
		uint64_t target_object = 0, target_lookup = 0, rebinds = 0;
		uint64_t relocated_hits = 0, relocated_rebinds = 0, relocated_unresolved = 0, relocated_miss = 0,
		         relocated_stored = 0, relocated_broken = 0;
		uint64_t budget_denied = 0, pipeline_pending = 0, relocated_instances = 0;
		// DSP-INSTR: time in the layer (ns) and calls.
		uint64_t ns_hit = 0, ns_try_miss = 0, ns_store = 0, n_hit = 0, n_try_miss = 0, n_store_calls = 0;
		std::array<uint64_t, 12> sec_ns {};
	} counters;
	// DSP-INSTR: why logs stopped validating: (pair, entry) -> count, and one example.
	struct Mismatch {
		uint64_t count = 0, address = 0;
		uint32_t size = 0, old_value = 0, new_value = 0, index = 0;
		uint64_t pair = 0;
	};
	std::unordered_map<uint64_t, Mismatch> mismatches;
	Counters last_counters;
	uint64_t last_report_frame = 0;
	std::unordered_map<std::string_view, uint64_t> refuse_reasons;
	~NativeXprCache() {
		for (auto pool: pools) device.destroyDescriptorPool(pool);
	}
};

namespace {

// DSP-INSTR: time of a section of the hit path (ns, summed into NativeXprCache::Counters::sec_ns).
struct NativeXprSection {
	uint64_t&                             sum;
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	explicit NativeXprSection(uint64_t& target): sum(target) {}
	~NativeXprSection() {
		sum += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	}
};

// The pair of shader addresses, mixed (a hash of two words is taken several times per draw).
uint64_t NativeXprPairHash(uint64_t vs, uint64_t ps) {
	uint64_t h = vs * 0x9e3779b97f4a7c15ull ^ (ps + 0x632be59bd9b4e019ull) * 0xc2b2ae3d27d4eb4full;
	h ^= h >> 29u;
	h *= 0xbf58476d1ce4e5b9ull;
	return h ^ (h >> 32u);
}

// The shader addresses of a key (its first four words).
uint64_t NativeXprPairKey(const std::vector<uint32_t>& key) {
	return key.size() < 4 ? 0
	                      : NativeXprPairHash(key[0] | uint64_t {key[1]} << 32u, key[2] | uint64_t {key[3]} << 32u);
}

// The pair of shader addresses, as NativeXprPairKey() hashes them.
uint64_t NativeXprPairOfShaders(const HW::Shader& sh) {
	const auto es = sh.GetVs().es_regs.data_addr;
	const auto vs = es != 0 ? es : sh.GetVs().gs_regs.data_addr;
	return NativeXprPairHash(vs, sh.GetPs().ps_regs.data_addr);
}

uint64_t NativeXprPairOf(const CommandBuffer& buffer) {
	return NativeXprPairOfShaders(buffer.GetShaders());
}

bool NativeXprResolveSkipEnabled() {
	static const bool enabled = NativeXprSwitch("KYTY_XPR_RESOLVE_SKIP");
	return enabled;
}

// The user-data SGPRs up to the last nonzero one (the registers carry no count).
uint32_t NativeXprUserSgprCount(const HW::UserSgprInfo& sgprs) {
	uint32_t count = HW::UserSgprInfo::SGPRS_MAX;
	while (count != 0 && sgprs.value[count - 1] == 0) --count;
	return count;
}

// Object identity: both shader addresses and every user-data SGPR (the SRT chains start there).
void NativeXprKeyOfShaders(const HW::Shader& sh, std::vector<uint32_t>& key) {
	const auto  es = sh.GetVs().es_regs.data_addr;
	const auto  vs = es != 0 ? es : sh.GetVs().gs_regs.data_addr;
	const auto  ps = sh.GetPs().ps_regs.data_addr;
	key.clear();
	key.push_back(static_cast<uint32_t>(vs));
	key.push_back(static_cast<uint32_t>(vs >> 32u));
	key.push_back(static_cast<uint32_t>(ps));
	key.push_back(static_cast<uint32_t>(ps >> 32u));
	for (const auto* sgprs: {&sh.GetVs().hs_user_sgpr, &sh.GetVs().gs_user_sgpr, &sh.GetPs().ps_user_sgpr}) {
		const auto count = NativeXprUserSgprCount(*sgprs);
		key.push_back(count);
		key.insert(key.end(), sgprs->value, sgprs->value + count);
	}
}

void NativeXprKeyOf(const CommandBuffer& buffer, std::vector<uint32_t>& key) {
	NativeXprKeyOfShaders(buffer.GetShaders(), key);
}

// Draw-state identity: the whole graphics context, the user configuration and the graphics shader
// stage registers with the user-data layout (types and counts) but not the user-data values,
// which identify the object instead.
// chenxiao07's state key cache (KYTY_STATE_KEY_CACHE, default on; 0: hashed at every call): the context and user
// configuration registers are hashed again only for other snapshots of them (the command processor's ids: the same
// id, the same registers).
bool StateKeyCacheEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_STATE_KEY_CACHE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

uint64_t NativeXprStateKeyOf(const CommandBuffer& buffer, StateKeyCache* cache = nullptr,
                             uint64_t context_id = 0, uint64_t user_config_id = 0) {
	const auto& ctx = buffer.GetRegisters();
	const auto& cfg = buffer.GetUserConfig();
	const auto& vs  = buffer.GetShaders().GetVs();
	const auto& ps  = buffer.GetShaders().GetPs();
	uint64_t    hash = 0;
	if (cache != nullptr && context_id != 0 && user_config_id != 0 && StateKeyCacheEnabled() &&
	    cache->context_id == context_id && cache->user_config_id == user_config_id) {
		hash = cache->hash;
	} else {
		hash = XXH3_64bits(&ctx, sizeof(ctx));
		hash = XXH3_64bits_withSeed(&cfg, sizeof(cfg), hash);
		if (cache != nullptr) *cache = {context_id, user_config_id, hash};
	}
	hash      = XXH3_64bits_withSeed(&vs.es_regs, sizeof(vs.es_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.ls_regs, sizeof(vs.ls_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.hs_regs, sizeof(vs.hs_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.gs_regs, sizeof(vs.gs_regs), hash);
	hash      = XXH3_64bits_withSeed(&ps.ps_regs, sizeof(ps.ps_regs), hash);
	return hash;
}

// What a relocated record is looked up by: a stage's program and descriptors.
struct NativeXprStageView {
	const ShaderRecompiler::IR::CompiledShaderInfo*           program  = nullptr;
	const std::vector<ShaderRecompiler::IR::DescriptorValue>* buffers  = nullptr;
	const std::vector<ShaderRecompiler::IR::DescriptorValue>* images   = nullptr;
	const std::vector<ShaderRecompiler::IR::DescriptorValue>* samplers = nullptr;
};

// The buffer descriptor dwords that do not hold the base address.
constexpr std::array<uint32_t, 8> NativeXprBufferShapeMask {0u, 0xffff0000u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};

// Full: every descriptor dword but the LOD statistics fields; shape: the buffer bases left out.
std::pair<uint64_t, uint64_t> NativeXprRelocationHashes(uint64_t vertex_program, uint64_t pixel_program,
                                                        std::span<const NativeXprStageView> stages) {
	const std::array<uint64_t, 3> ids {vertex_program, pixel_program, stages.size()};
	auto full  = XXH3_64bits(ids.data(), sizeof(ids));
	auto shape = full;
	const auto mix = [](uint64_t seed, const std::vector<ShaderRecompiler::IR::DescriptorValue>& values,
	                    const std::array<uint32_t, 8>* mask) {
		std::array<uint32_t, 9> words {};
		for (const auto& value: values) {
			words[0] = value.dword_count;
			for (uint32_t w = 0; w < 8; ++w) words[1 + w] = value.dwords[w] & (mask != nullptr ? (*mask)[w] : ~0u);
			seed = XXH3_64bits_withSeed(words.data(), sizeof(words), seed);
		}
		const uint64_t count = values.size();
		return XXH3_64bits_withSeed(&count, sizeof(count), seed);
	};
	std::array<uint32_t, 8> image_mask {};
	for (uint32_t w = 0; w < 8; ++w) image_mask[w] = ~NativeXprLodStatsMask[w];
	for (const auto& stage: stages) {
		const auto program = reinterpret_cast<uintptr_t>(stage.program);
		full  = XXH3_64bits_withSeed(&program, sizeof(program), full);
		shape = XXH3_64bits_withSeed(&program, sizeof(program), shape);
		full  = mix(full, *stage.buffers, nullptr);
		shape = mix(shape, *stage.buffers, &NativeXprBufferShapeMask);
		full  = mix(mix(full, *stage.images, &image_mask), *stage.samplers, nullptr);
		shape = mix(mix(shape, *stage.images, &image_mask), *stage.samplers, nullptr);
	}
	return {full, shape};
}

} // namespace

RenderExecutor::NativeXprCache& RenderExecutor::NativeXprState() {
	if (!m_native_xpr) {
		m_native_xpr = {new NativeXprCache {}, [](NativeXprCache* cache) { delete cache; }};
		m_native_xpr->device  = m_context.GetGraphics().device;
		m_native_xpr->scratch = {new DrawRenderStorage {}, [](DrawRenderStorage* storage) { delete storage; }};
	}
	return *m_native_xpr;
}

vk::DescriptorSet RenderExecutor::NativeXprAllocateSet(vk::DescriptorSetLayout layout,
                                                       vk::DescriptorPool&     pool_out) {
	auto&      cache  = NativeXprState();
	const auto device = cache.device;
	if (const auto found = cache.free_sets.find(static_cast<VkDescriptorSetLayout>(layout));
	    found != cache.free_sets.end() && !found->second.empty()) {
		const auto item = found->second.back();
		found->second.pop_back();
		--cache.free_count;
		pool_out = item.pool;
		return item.set;
	}
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &layout;
	vk::DescriptorSet set       = nullptr;
	// Freed sets return to their pool; try the newest pools first.
	for (size_t i = 0; i < cache.pools.size() && i < 4; ++i) {
		const auto pool         = cache.pools[cache.pools.size() - 1 - i];
		allocate.descriptorPool = pool;
		if (device.allocateDescriptorSets(&allocate, &set) == vk::Result::eSuccess) {
			pool_out = pool;
			cache.set_layouts[static_cast<VkDescriptorSet>(set)] = static_cast<VkDescriptorSetLayout>(layout);
			return set;
		}
	}
	constexpr uint32_t Sets = 1024;
	const std::array<vk::DescriptorPoolSize, 5> sizes {{
	    {vk::DescriptorType::eStorageBuffer, Sets * 16},
	    {vk::DescriptorType::eStorageBufferDynamic, Sets * 4},
	    {vk::DescriptorType::eSampledImage, Sets * 32},
	    {vk::DescriptorType::eSampler, Sets * 8},
	    {vk::DescriptorType::eStorageImage, Sets * 4},
	}};
	vk::DescriptorPoolCreateInfo create {};
	create.flags         = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	create.maxSets       = Sets;
	create.poolSizeCount = static_cast<uint32_t>(sizes.size());
	create.pPoolSizes    = sizes.data();
	vk::DescriptorPool pool = nullptr;
	EXIT_IF(device.createDescriptorPool(&create, nullptr, &pool) != vk::Result::eSuccess);
	cache.pools.push_back(pool);
	allocate.descriptorPool = pool;
	if (device.allocateDescriptorSets(&allocate, &set) != vk::Result::eSuccess) {
		return nullptr; // a set larger than a pool: the record is refused
	}
	pool_out = pool;
	cache.set_layouts[static_cast<VkDescriptorSet>(set)] = static_cast<VkDescriptorSetLayout>(layout);
	return set;
}

// A descriptor set leaves service; it may be in flight until the current tick.
void RenderExecutor::NativeXprRetireSet(vk::DescriptorPool pool, vk::DescriptorSet set) {
	if (!set) return;
	TableMemoRetired(); // (a memo entry may hold it)
	NativeXprState().retired.push_back({pool, set, m_context.GetCommandScheduler().CurrentTick()});
}

// A record leaves service.
void RenderExecutor::NativeXprRetire(std::unique_ptr<NativeXprRecord> record) {
	NativeXprRetireSet(record->pool, record->set);
	record->set = nullptr;
	--NativeXprState().record_count;
}

// The draw states no draw used for StateKeepFrames leave. What holds one by its address forgets it: this executor's
// emission and lookup, its table's last draw and its image sets' transits (TableXpr::epoch moves). Not while a
// speculation translates or has a result unfinished (kyty_local_speculations_live): it holds them across draws too.
// False: put off for that, tried again at the next call.
bool RenderExecutor::NativeXprSweepStates(NativeXprCache& cache, uint64_t frame) {
	if (Role() != 0 || cache.states.size() <= NativeXprCache::StatesKept) return true;
	if (kyty_local_speculations_live.load(std::memory_order_seq_cst) != 0) return false;
	const auto write = CatalogWrite();
	// (Under the lock: a translation counted after this reads the catalog after the sweep, and sees its epoch.)
	if (kyty_local_speculations_live.load(std::memory_order_seq_cst) != 0) return false;
	const size_t before = cache.states.size();
	std::erase_if(cache.states, [&](const auto& item) {
		return item.second->UsedFrame() + NativeXprCache::StateKeepFrames < frame;
	});
	if (cache.states.size() == before) return true;
	cache.emit           = {};
	cache.last_state     = nullptr;
	cache.last_state_key = 0;
	TableForgetStates();
	return true;
}

// Once per frame: records no draw used for a while leave; the retired sets the GPU is done with
// are kept for reuse or freed.
void RenderExecutor::NativeXprCollect() {
	if (!m_native_xpr) return;
	auto&      cache = *m_native_xpr;
	const auto frame = m_context.FrameNumber();
	if (NativeXprStatsEnabled() && frame >= cache.reported_frame + 300) {
		cache.reported_frame = frame;
		const auto& c        = cache.counters;
		std::printf("NativeXpr: frame %llu records %zu states %zu tries %llu hits %llu miss key %llu state %llu "
		            "target %llu validate %llu emit %llu stores %llu stored %llu refused %llu\n",
		            static_cast<unsigned long long>(frame), cache.record_count, cache.states.size(),
		            static_cast<unsigned long long>(c.tries), static_cast<unsigned long long>(c.hits),
		            static_cast<unsigned long long>(c.miss_key), static_cast<unsigned long long>(c.miss_state),
		            static_cast<unsigned long long>(c.miss_target),
		            static_cast<unsigned long long>(c.miss_validate), static_cast<unsigned long long>(c.miss_emit),
		            static_cast<unsigned long long>(c.stores), static_cast<unsigned long long>(c.stored),
		            static_cast<unsigned long long>(c.refused));
		for (const auto& [reason, count]: cache.refuse_reasons)
			std::printf("NativeXpr:   refused %.*s %llu\n", static_cast<int>(reason.size()), reason.data(),
			            static_cast<unsigned long long>(count));
		std::printf("NativeXpr:   validate unmapped %llu read %llu unlearned %llu resources %llu\n",
		            static_cast<unsigned long long>(c.validate_unmapped), static_cast<unsigned long long>(c.validate_read),
		            static_cast<unsigned long long>(c.validate_unlearned),
		            static_cast<unsigned long long>(c.validate_resources));
		std::printf("NativeXpr:   learn ok %llu reads %llu programs %llu buffers %llu images %llu samplers %llu other %llu "
		            "unexplained %llu; target object %llu lookup %llu; rebinds %llu\n",
		            static_cast<unsigned long long>(c.learn[0]), static_cast<unsigned long long>(c.learn[1]),
		            static_cast<unsigned long long>(c.learn[2]), static_cast<unsigned long long>(c.learn[3]),
		            static_cast<unsigned long long>(c.learn[4]), static_cast<unsigned long long>(c.learn[5]),
		            static_cast<unsigned long long>(c.learn[6]), static_cast<unsigned long long>(c.learn[7]),
		            static_cast<unsigned long long>(c.target_object), static_cast<unsigned long long>(c.target_lookup),
		            static_cast<unsigned long long>(c.rebinds));
		{
			const auto frames = std::max<uint64_t>(1, frame - cache.last_report_frame);
			const auto& l = cache.last_counters;
			std::printf("NativeXpr:   per frame since last: hit %llu calls %.1f us, try-miss %llu calls %.1f us, store %llu calls %.1f us\n",
			            static_cast<unsigned long long>((c.n_hit - l.n_hit) / frames), double(c.ns_hit - l.ns_hit) / 1000.0 / frames,
			            static_cast<unsigned long long>((c.n_try_miss - l.n_try_miss) / frames), double(c.ns_try_miss - l.ns_try_miss) / 1000.0 / frames,
			            static_cast<unsigned long long>((c.n_store_calls - l.n_store_calls) / frames), double(c.ns_store - l.ns_store) / 1000.0 / frames);
			std::printf("NativeXpr:   hit sections per frame (us): validate %.1f (stillvalid %.1f) uploads %.1f obtain %.1f targets %.1f images %.1f vk %.1f\n",
			            double(c.sec_ns[0] - l.sec_ns[0]) / 1000.0 / frames, double(c.sec_ns[1] - l.sec_ns[1]) / 1000.0 / frames,
			            double(c.sec_ns[2] - l.sec_ns[2]) / 1000.0 / frames, double(c.sec_ns[3] - l.sec_ns[3]) / 1000.0 / frames,
			            double(c.sec_ns[4] - l.sec_ns[4]) / 1000.0 / frames, double(c.sec_ns[5] - l.sec_ns[5]) / 1000.0 / frames,
			            double(c.sec_ns[6] - l.sec_ns[6]) / 1000.0 / frames);
			std::printf("NativeXpr:   prelude per frame (us): pop+lock+collect %.1f key+state+hash %.1f post-validate %.1f\n",
			            double(c.sec_ns[8] - l.sec_ns[8]) / 1000.0 / frames, double(c.sec_ns[7] - l.sec_ns[7]) / 1000.0 / frames,
			            double(c.sec_ns[9] - l.sec_ns[9]) / 1000.0 / frames);
			cache.last_counters = c;
			cache.last_report_frame = frame;
			std::vector<std::pair<uint64_t, uint64_t>> top;
			for (const auto& [key, item]: cache.mismatches) top.emplace_back(item.count, key);
			std::ranges::sort(top, std::greater<>());
			for (size_t i = 0; i < top.size() && i < 8; ++i) {
				const auto& item = cache.mismatches[top[i].second];
				std::printf("NativeXpr:   mismatch pair %016llx entry %u addr 0x%llx size %u count %llu old %08x new %08x\n",
				            static_cast<unsigned long long>(item.pair), item.index, static_cast<unsigned long long>(item.address),
				            item.size, static_cast<unsigned long long>(item.count), item.old_value, item.new_value);
			}
		}
		std::printf("NativeXpr:   relocated records %zu hits %llu rebinds %llu unresolved %llu miss %llu stored %llu "
		            "broken %llu\n",
		            cache.relocated.size(), static_cast<unsigned long long>(c.relocated_hits),
		            static_cast<unsigned long long>(c.relocated_rebinds),
		            static_cast<unsigned long long>(c.relocated_unresolved),
		            static_cast<unsigned long long>(c.relocated_miss), static_cast<unsigned long long>(c.relocated_stored),
		            static_cast<unsigned long long>(c.relocated_broken));
		std::fflush(stdout);
	}
	if (frame - cache.swept_frame >= 8) {
		cache.swept_frame = frame;
		const uint64_t KeepFrames =
		    cache.record_count > NativeXprCache::RecordsSoftCap ? uint64_t {2} : NativeXprKeepFrames();
		for (auto it = cache.records.begin(); it != cache.records.end();) {
			auto& variants = it->second;
			for (auto variant = variants.begin(); variant != variants.end();) {
				if ((*variant)->used_frame + KeepFrames >= frame) {
					++variant;
					continue;
				}
				NativeXprRetire(std::move(*variant));
				variant = variants.erase(variant);
			}
			it = variants.empty() ? cache.records.erase(it) : std::next(it);
		}
		std::vector<NativeXprRecord*> old;
		for (auto& [full, record]: cache.relocated)
			if (record->used_frame + KeepFrames < frame) old.push_back(record.get());
		for (auto* record: old) NativeXprDropRelocated(*record);
		std::erase_if(cache.refused, [&](const auto& item) { return item.second + NativeXprCache::RefuseFrames <= frame; });
	}
	// (Put off while a speculation holds the states: tried at every call until done.)
	if (frame - cache.states_swept_frame >= NativeXprCache::StateSweepFrames && NativeXprSweepStates(cache, frame))
		cache.states_swept_frame = frame;
	if (cache.collected_frame == frame || cache.retired.empty()) return;
	cache.collected_frame = frame;
	auto& scheduler       = m_context.GetCommandScheduler();
	std::vector<NativeXprCache::RetiredSet> done;
	std::erase_if(cache.retired, [&](const NativeXprCache::RetiredSet& item) {
		if (!scheduler.IsFree(item.tick)) return false;
		const auto layout = cache.set_layouts.find(static_cast<VkDescriptorSet>(item.set));
		if (layout != cache.set_layouts.end() && cache.free_count < NativeXprCache::FreeSetsMax) {
			auto& sets = cache.free_sets[layout->second];
			if (sets.size() < NativeXprCache::FreeSetsPerLayout) {
				sets.push_back({item.pool, item.set});
				++cache.free_count;
				return true;
			}
		}
		if (layout != cache.set_layouts.end()) cache.set_layouts.erase(layout);
		done.push_back(item);
		return true;
	});
	std::ranges::sort(done, {}, [](const auto& item) { return static_cast<VkDescriptorPool>(item.pool); });
	std::vector<vk::DescriptorSet> batch;
	for (size_t first = 0; first < done.size();) {
		size_t last = first;
		batch.clear();
		while (last < done.size() && done[last].pool == done[first].pool) batch.push_back(done[last++].set);
		(void)cache.device.freeDescriptorSets(done[first].pool, static_cast<uint32_t>(batch.size()), batch.data());
		first = last;
	}
}

// A fresh record replaced a stale one whose logged reads were at the same addresses. A position of
// the evaluation's output (a flattened SRT entry, or a buffer descriptor's base address) that held
// a logged word in both evaluations follows that word: its next change is patched there. Changed
// words that reach no position reach nothing (they are ignored); a moved position no word explains
// means the output is not a plain copy of the words, and nothing is learned.
// The resolved bindings of the normal draw -> record: the stage data, the runtime proof
// dependencies and the descriptor set. Returns a reject reason, or nullptr.
const char* RenderExecutor::NativeXprBindResources(NativeXprRecord& record,
                                                   std::span<PreparedBindings* const> stages,
                                                   const PipelineCache::Pipeline& pipeline) {
	using Kind      = ShaderRecompiler::IR::DescriptorBindingKind;
	auto& buffers   = m_context.GetBufferCache();
	auto& textures  = m_context.GetTextureCache();
	const auto& graphics = m_context.GetGraphics();
	for (const auto* prepared: stages) {
		const auto& program = prepared->runtime->module->info;
		// (A record syncs no device-address reads: a program reading memory through them takes the normal path, which
		// synchronizes them.)
		if (prepared->gds.buffer != nullptr || program.has_address_writes || program.info.uses_dma)
			return "dma_gds_or_address_writes";
		if (!prepared->table_images.empty() || prepared->table_state != nullptr || prepared->tables_reused)
			return "image_tables";
		for (const auto& resource: program.info.buffers)
			if (resource.written || resource.atomic) return "written_buffer";
		for (const auto& image: program.info.images)
			if (image.written || image.atomic) return "written_image";
		for (const auto& image: prepared->images)
			if (image.desc.type == TextureCache::BindingType::Storage) return "storage_image";
	}
	std::vector<NativeXprRecord::BufferDep> buffer_deps;
	std::vector<NativeXprRecord::ImageDep>  image_deps;
	const auto alignment = graphics.StorageMinAlignment();
	const auto max_range = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	for (uint32_t st = 0; st < stages.size(); ++st) {
		auto* const prepared = stages[st];
		const auto& program  = prepared->runtime->module->info;
		for (uint32_t r = 0; r < program.info.buffers.size(); ++r) {
			const auto& resource = program.info.buffers[r];
			const auto  index    = resource.descriptor_index;
			if (index == UINT32_MAX) continue;
			const auto& source = prepared->buffer_sources[index];
			if (source.address == 0 || source.size == 0) continue;
			const bool texel = resource.formatted || (resource.read && Config::SyncRawImageBuffersEnabled());
			auto id = source.id;
			if (!id || buffers.GetBufferIfLive(id) == nullptr ||
			    !buffers.GetBufferIfLive(id)->IsInBounds(source.address, source.size))
				id = buffers.FindBuffer(source.address, source.size);
			const auto* owner = buffers.GetBufferIfLive(id);
			if (owner == nullptr || !owner->IsInBounds(source.address, source.size)) return "buffer_owner";
			if (prepared->buffers[index].buffer != owner->Handle() ||
			    buffers.IsRegionCpuModified(source.address, source.size)) {
				// The normal path streams small ranges (CPU-dirty pages, or a copy it made earlier in
				// the draw window) and never cleans the owner. Clean it once and bind the owner: a
				// later CPU write marks the range dirty and the record's proof notices.
				if (buffers.IsRegionGpuModified(source.address, source.size)) return "buffer_gpu_and_cpu";
				buffers.EnsureBufferContents(source.address, source.size);
				if (buffers.IsRegionCpuModified(source.address, source.size)) return "buffer_still_dirty";
				const auto offset     = owner->Offset(source.address);
				const auto aligned    = offset - offset % alignment;
				const auto adjustment = offset - aligned;
				if (adjustment >= 256 || source.size > max_range - adjustment) return "buffer_alignment";
				prepared->buffers[index] = {owner->Handle(), aligned, source.size + adjustment};
				auto&      word  = prepared->shader_data[program.bindings.memory_offset_dword + index / 4u];
				const auto shift = (index % 4u) * 8u;
				word = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(adjustment) << shift);
			}
			// ObtainBuffer refreshes a texel buffer from an image that starts exactly at its address;
			// the record cannot.
			if (texel && textures.HasImageStartingAt(source.address)) return "texel_image";
			buffer_deps.push_back({source.address, source.size, id, owner->Handle(), texel, st, r, index});
		}
		for (const auto& image: prepared->images) {
			// A null image is bound as the normal path binds it.
			if (!image.image_id) continue;
			if (image.layout != vk::ImageLayout::eShaderReadOnlyOptimal) return "image_layout";
			const auto* owner = textures.m_slot_images.try_get(image.image_id);
			if (owner == nullptr || owner->binding.is_target || owner->binding.force_general)
				return "image_binding";
			if (!textures.IsSampledImageCurrent(image.image_id, image.desc)) return "image_not_current";
			const auto& view = image.desc.view_info;
			image_deps.push_back({image.image_id, owner->serial, owner->registrations, image.layout,
			                      {view.base_level, view.level_count, view.base_layer, view.layer_count},
			                      image.desc});
		}
	}

	// Stage data and the descriptor payload, in the order CommitBindings() writes them.
	struct Pending {
		vk::WriteDescriptorSet write;
		size_t                 start = 0;
		bool                   image = false;
	};
	std::vector<Pending>                  pending;
	std::vector<vk::DescriptorBufferInfo> buffer_infos;
	std::vector<vk::DescriptorImageInfo>  image_infos;
	std::vector<NativeXprRecord::DynamicBinding> dynamic_bindings;
	const auto upload = buffers.GetUtilityBuffer(MemoryUsage::Stream).Handle();
	vk::ShaderStageFlags push_stages = vk::ShaderStageFlagBits::eFragment;
	bool                 has_push    = false;
	for (uint32_t index = 0; index < stages.size(); ++index) {
		const auto& prepared = *stages[index];
		const auto& program  = prepared.runtime->module->info;
		const auto& snapshot = *prepared.runtime->resources;
		auto&       stage    = record.stages[index];
		stage.program        = &program;
		stage.shader_data    = prepared.shader_data;
		record.snapshots[index] = snapshot;
		stage.srt_binding = stage.data_binding = false;
		stage.push_start = ShaderRecompiler::IR::PushData::NoStart;
		if (stage.shader_data.size() != program.bindings.ShaderDataDwords()) return "shader_data";
		if (program.bindings.UsesPushData()) {
			stage.push_start = program.bindings.push_data_start_dword;
			has_push         = true;
		}
		push_stages |= NativeShaderStage(program.stage);
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
			} else {
				switch (kind) {
					case Kind::Buffers:
						item.start = buffer_infos.size();
						record.buffers_start[index] = static_cast<uint32_t>(item.start);
						if (prepared.buffers.size() != item.write.descriptorCount) return "buffer_count";
						for (const auto& view: prepared.buffers) {
							if (view.buffer == nullptr) return "null_buffer";
							buffer_infos.push_back(view);
						}
						break;
					case Kind::UniformBuffers:
						item.start = buffer_infos.size();
						if (prepared.uniform_buffers.size() != item.write.descriptorCount) return "uniform_count";
						for (const auto& view: prepared.uniform_buffers) {
							if (view.buffer == nullptr || view.range == 0) return "null_uniform";
							buffer_infos.push_back(view);
						}
						break;
					case Kind::MipStats: {
						const auto& counters = m_context.GetLodStats().CounterBuffer();
						item.start = buffer_infos.size();
						buffer_infos.emplace_back(counters.Handle(), 0, counters.Size());
						break;
					}
					case Kind::BdaPagetable:
					case Kind::FaultBuffer: {
						const auto* owner = kind == Kind::BdaPagetable ? buffers.GetBdaPageTableBuffer()
						                                               : buffers.GetFaultBuffer();
						item.start = buffer_infos.size();
						buffer_infos.emplace_back(owner->Handle(), 0, owner->Size());
						break;
					}
					case Kind::FlattenedSrt:
					case Kind::ShaderData: {
						const bool  srt    = kind == Kind::FlattenedSrt;
						const auto& values = srt ? record.snapshots[index].flattened_srt : stage.shader_data;
						if (values.empty() || item.write.descriptorCount != 1) return "empty_dynamic";
						(srt ? stage.srt_binding : stage.data_binding) = true;
						item.write.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
						item.start                = buffer_infos.size();
						buffer_infos.emplace_back(upload, 0, values.size() * sizeof(uint32_t));
						dynamic_bindings.push_back({item.write.dstBinding, index, srt});
						break;
					}
					case Kind::Samplers:
						item.image = true;
						item.start = image_infos.size();
						for (const auto sampler: prepared.samplers) {
							if (sampler == nullptr) return "null_sampler";
							image_infos.emplace_back(sampler, nullptr, vk::ImageLayout::eUndefined);
						}
						if (prepared.samplers.size() != item.write.descriptorCount) return "sampler_count";
						break;
					default: return "binding_kind";
				}
			}
			pending.push_back(item);
		}
		if (prepared.images.size() != program.info.images.size()) return "image_count";
		for (uint32_t i = 0; i < program.info.images.size(); ++i) {
			const auto& resource = program.info.images[i];
			const auto& texture  = prepared.images[i];
			if (texture.image_id && resource.mip_count != (texture.mip_views.empty() ? 1u : texture.mip_views.size()))
				return "image_mips";
			const auto kind  = *ShaderRecompiler::IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] + resource.descriptor_index;
			for (uint32_t mip = 0; mip < resource.mip_count; ++mip) {
				if (start + mip >= image_infos.size()) return "image_slot";
				image_infos[start + mip] = MakeImageInfo(texture, mip);
			}
		}
	}
	// The dynamic offsets follow binding order.
	std::ranges::sort(dynamic_bindings, {}, &NativeXprRecord::DynamicBinding::binding);

	record.stage_count      = static_cast<uint32_t>(stages.size());
	record.push_stages      = push_stages;
	record.has_push         = has_push;
	record.upload           = upload;
	record.dynamic_bindings = std::move(dynamic_bindings);
	record.buffers          = std::move(buffer_deps);
	record.images           = std::move(image_deps);
	record.mapping_version  = m_context.MappedRangesVersion();
	record.buffer_set_epoch = buffers.BufferSetEpoch();
	record.image_set_epoch  = textures.ImageSetEpoch();
	uint64_t cpu_epoch      = 0;
	for (const auto& dep: record.buffers) {
		const auto epoch = buffers.CpuDirtyEpoch(dep.address, dep.size);
		if (epoch == 0) {
			cpu_epoch = 0;
			break;
		}
		cpu_epoch += epoch;
	}
	record.cpu_epoch         = cpu_epoch;
	record.image_dirty_epoch = Image::CpuDirtyEpoch();
	record.validated_frame   = m_context.FrameNumber();

	record.set = NativeXprAllocateSet(pipeline.descriptor_set_layout, record.pool);
	if (!record.set) return "set_allocation";
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(pending.size());
	for (auto& item: pending) {
		item.write.dstSet = record.set;
		if (item.image) item.write.pImageInfo = image_infos.data() + item.start;
		else item.write.pBufferInfo = buffer_infos.data() + item.start;
		writes.push_back(item.write);
	}
	if (!writes.empty())
		m_context.GetGraphics().device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
		                                                    nullptr);
	record.set_writes.clear();
	for (const auto& item: pending)
		record.set_writes.push_back({item.write.dstBinding, item.write.descriptorCount, static_cast<uint32_t>(item.start),
		                             item.write.descriptorType, item.image});
	record.set_buffers = std::move(buffer_infos);
	record.set_images  = std::move(image_infos);
	return nullptr;
}

// Called by ExecutePreparedDraw() after the draw is recorded, for a draw whose try requested a
// store.
void RenderExecutor::NativeXprStore(CommandBuffer& buffer, const DrawRenderState& state,
                                    vk::PrimitiveTopology topology, bool primitive_restart_enable,
                                    const RenderState& rendering, std::span<PreparedBindings* const> stages,
                                    const DynamicState& dynamic_state) {
	auto& cache = NativeXprState();
	++cache.counters.stores;
	struct StoreTimer {
		NativeXprCache&                       cache;
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		~StoreTimer() {
			cache.counters.ns_store += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			++cache.counters.n_store_calls;
		}
	} store_timer {cache};
	const auto frame  = m_context.FrameNumber();
	const bool relocated = cache.store_relocated;
	const auto refuse    = [&](const char* reason) {
		++cache.counters.refused;
		cache.refused[relocated ? cache.relocated_full : cache.tried_hash] = frame;
		++cache.refuse_reasons[reason];
	};
	const auto refuse_program = [&](const char* reason) {
		refuse(reason);
		std::vector<uint32_t> key;
		NativeXprKeyOf(buffer, key);
		cache.unstorable[NativeXprPairKey(key)] = true;
	};
	if (stages.empty() || stages.size() > 2 || (stages.size() == 2) != state.ps_active ||
	    state.VertexStages().size() != 1) {
		return refuse_program("stages");
	}
	for (const auto* prepared: stages) {
		const auto& program = prepared->runtime->module->info;
		if (prepared->gds.buffer != nullptr || program.has_address_writes || program.info.uses_dma)
			return refuse_program("dma_gds");
		for (const auto& resource: program.info.buffers)
			if (resource.written || resource.atomic) return refuse_program("written_buffer");
		for (const auto& image: program.info.images)
			if (image.written || image.atomic) return refuse_program("written_image");
	}
	auto record = std::make_unique<NativeXprRecord>();
	NativeXprKeyOf(buffer, record->key);
	if (NativeXprHash(record->key) != cache.tried_hash) return refuse("key_moved");
	const auto state_key = cache.state_key;
	if (relocated) {
		for (const auto* prepared: stages)
			if (!prepared->runtime->resources->image_tables.empty()) return refuse("image_tables");
		return NativeXprStoreRecord(std::move(record), buffer, state, topology, primitive_restart_enable, rendering,
		                            stages, dynamic_state, nullptr);
	}

	// The reads of the programs' SRT evaluation: the same resolution again, with a read log (the
	// draw's own resolution may not have one), which must give the same resources.
	auto& scratch = cache.scratch->state;
	ResetDrawRenderState(scratch, cache.scratch->tessellation_stages_written);
	cache.scratch->tessellation_stages_written = false;
	cache.scratch_programs.Reset();
	scratch.indirect_offsets = state.indirect_offsets;
	scratch.indirect_draw    = state.indirect_draw;
	if (!m_context.GetPipelineCache().GetGraphicsProgramsAhead(buffer.GetRegisters(), buffer.GetUserConfig(),
	                                                          buffer.GetShaders(), scratch,
	                                                          cache.scratch_programs) ||
	    scratch.vertex_info[0].stage.module == nullptr)
		return refuse("resolve");
	auto& reads = cache.scratch_programs.reads;
	if (reads.Failed() || !reads.DwordAligned() || reads.WordCount() > 4096) return refuse("reads");
	if (scratch.ps_active != state.ps_active ||
	    scratch.vertex_info[0].stage.module->id != state.vertex_info[0].stage.module->id ||
	    (state.ps_active && scratch.ps_input_info.stage.module->id != state.ps_input_info.stage.module->id))
		return refuse("programs_moved");
	const std::array<const ShaderStageRuntime*, 2> fresh_stages {&scratch.vertex_info[0].stage,
	                                                             &scratch.ps_input_info.stage};
	for (uint32_t st = 0; st < stages.size(); ++st) {
		const auto& a = *stages[st]->runtime->resources;
		const auto& b = *fresh_stages[st]->resources;
		if (fresh_stages[st]->module != stages[st]->runtime->module || a.buffers != b.buffers ||
		    a.images != b.images || a.samplers != b.samplers || a.flattened_srt != b.flattened_srt ||
		    a.user_data != b.user_data || !b.image_tables.empty())
			return refuse("resources_moved");
	}
	for (uint32_t st = 0; st < stages.size(); ++st) {
		const auto& snapshot = *fresh_stages[st]->resources;
		if (snapshot.uniform_fill.kind != ShaderRecompiler::IR::UniformFillKind::None || !snapshot.gpu_flat_loads.empty())
			return refuse_program("gpu_flat");
	}
	NativeXprStoreRecord(std::move(record), buffer, state, topology, primitive_restart_enable, rendering, stages,
	                     dynamic_state, &reads);
}

// The record of a store: its pipeline, resources and draw state; `reads` the SRT evaluation's
// read log, null for a relocated record.
void RenderExecutor::NativeXprStoreRecord(std::unique_ptr<NativeXprRecord> record, CommandBuffer& buffer,
                                          const DrawRenderState& state, vk::PrimitiveTopology topology,
                                          bool primitive_restart_enable, const RenderState& rendering,
                                          std::span<PreparedBindings* const> stages,
                                          const DynamicState& dynamic_state, const GuestReadLog* reads) {
	auto&      cache     = *m_native_xpr;
	const auto frame     = m_context.FrameNumber();
	const auto state_key = cache.state_key;
	const bool relocated = reads == nullptr;
	const auto refuse    = [&](const char* reason) {
		++cache.counters.refused;
		cache.refused[relocated ? cache.relocated_full : cache.tried_hash] = frame;
		++cache.refuse_reasons[reason];
	};
	auto* const pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(buffer, state, *m_draw_targets, NativeXprAsyncPipelines(), true);
	if (pipeline == nullptr && NativeXprAsyncPipelines()) {
		++cache.counters.pipeline_pending; // a background thread creates it: a later store takes it
		return;
	}
	if (pipeline == nullptr || !pipeline->native_bindings || pipeline->uses_push_descriptors) return refuse("pipeline");
	record->vertex_program = state.vertex_info[0].stage.module->id;
	record->fetch_embedded = false; // (direct draws are not stored)
	record->vertex_buffers = state.vertex_info[0].buffers_num > 0;
	record->pixel_program  = state.ps_active ? state.ps_input_info.stage.module->id : 0;
	record->pipelines.emplace_back(state_key, pipeline);
	if (const char* reason = NativeXprBindResources(*record, stages, *pipeline)) {
		NativeXprRetireSet(record->pool, record->set);
		return refuse(reason);
	}
	if (reads != nullptr) {
		record->reads = *reads;
		// The read log's descriptor words and flat loads name the evaluation's own resources.
		for (uint32_t st = 0; st < stages.size(); ++st) {
			record->snapshots[st] = cache.scratch_programs.resources[st];
			record->plans[st]     = cache.scratch_programs.plans[st];
		}
	}

	// The draw state: the targets as this draw left them and the dynamic state.
	NativeXprCaptureState(state, rendering, state_key, dynamic_state);

	if (relocated) {
		std::array<NativeXprStageView, 2> views {};
		for (uint32_t st = 0; st < record->stage_count; ++st) {
			const auto& stage = record->stages[st];
			views[st]         = {stage.program, &record->snapshots[st].buffers, &record->snapshots[st].images, &record->snapshots[st].samplers};
		}
		const auto [full, shape] = NativeXprRelocationHashes(record->vertex_program, record->pixel_program,
		                                                     std::span {views.data(), record->stage_count});
		record->relocated  = true;
		record->full_hash  = full;
		record->shape_hash = shape;
		record->used_frame = frame;
		if (const auto other = cache.relocated.find(full); other != cache.relocated.end()) {
			// The record of other draw states: its pipelines stay.
			if (other->second->vertex_program == record->vertex_program &&
			    other->second->pixel_program == record->pixel_program)
				for (const auto& entry: other->second->pipelines)
					if (std::ranges::find(record->pipelines, entry.first,
					                      &std::pair<uint64_t, const PipelineCache::Pipeline*>::first) ==
					    record->pipelines.end())
						record->pipelines.push_back(entry);
			NativeXprDropRelocated(*other->second);
		}
		cache.relocated_shapes[shape] = record.get();
		++cache.pairs[NativeXprPairKey(record->key)].stored;
		cache.relocated.emplace(full, std::move(record));
		++cache.record_count;
		++cache.counters.relocated_stored;
		++cache.counters.stored;
		return;
	}

	const auto hash    = NativeXprHash(record->key);
	record->used_frame = frame;
	auto& variants = cache.records[hash];
	if (!variants.empty() && variants.front()->key != record->key) {
		// A hash collision: the other object's records leave.
		for (auto& variant: variants) NativeXprRetire(std::move(variant));
		variants.clear();
	}
	// Pipelines depend on the programs and the draw state only: every variant shares them.
	for (const auto& variant: variants)
		if (variant->vertex_program == record->vertex_program && variant->pixel_program == record->pixel_program)
			for (const auto& entry: variant->pipelines)
				if (std::ranges::find(record->pipelines, entry.first,
				                      &std::pair<uint64_t, const PipelineCache::Pipeline*>::first) == record->pipelines.end())
					record->pipelines.push_back(entry);
	// A variant whose words do not hold now may hold again (a texture that alternates between two images keeps a
	// record per image): the least recently used variant leaves when there are too many.
	if (variants.size() >= NativeXprCache::MaxVariants) {
		NativeXprRetire(std::move(variants.back()));
		variants.pop_back();
	}
	++cache.pairs[NativeXprPairKey(record->key)].stored;
	variants.insert(variants.begin(), std::move(record));
	++cache.record_count;
	++cache.counters.stored;
}

// The draw state of a stored draw (a native record's or a table draw's): the targets as the draw left them, their
// rendering without clears and the dynamic state. A state already known is written over in place.
void RenderExecutor::NativeXprCaptureState(const DrawRenderState& state, const RenderState& rendering,
                                           uint64_t state_key, const DynamicState& dynamic_state) {
	auto&               cache    = NativeXprState();
	auto&               textures = m_context.GetTextureCache();
	NativeXprDrawState  draw_state;
	draw_state.rendering = rendering;
	draw_state.rendering.depth_stencil_attachment.depth_clear   = false;
	draw_state.rendering.depth_stencil_attachment.stencil_clear = false;
	const auto metadata_of = [](const TextureCache::ImageDesc& desc) {
		return desc.info.metadata.kind == ImageMetadataKind::None ? uint64_t {0} : desc.info.metadata.range.address;
	};
	// A target the draw also sampled has the access its CommitBindings gave it (the attachment's and the shader's
	// reads): a table draw's image set may then hold it (KYTY_TABLE_TARGET_READS).
	const auto sampled_access = [](const Image& image) {
		return image.binding.is_bound && !image.binding.shader_write
		           ? image.binding.attachment_access | vk::AccessFlagBits2::eShaderRead
		           : image.binding.attachment_access;
	};
	for (uint32_t i = 0; i < m_draw_targets->color_count; ++i) {
		const auto& target = m_draw_targets->colors[i];
		const auto& view   = target.desc.view_info;
		const auto& image  = textures.GetImage(target.image_id);
		draw_state.targets.push_back({target.image_id, image.serial,
		                              rendering.color_attachments[target.target_slot].image_layout,
		                              sampled_access(image),
		                              {view.base_level, view.level_count, view.base_layer, view.layer_count},
		                              metadata_of(target.desc), view.base_layer, target.desc.info, view});
	}
	if (m_draw_targets->depth.image_id) {
		const auto& view  = m_draw_targets->depth.desc.view_info;
		const auto& image = textures.GetImage(m_draw_targets->depth.image_id);
		draw_state.targets.push_back({m_draw_targets->depth.image_id, image.serial,
		                              rendering.depth_stencil_attachment.image_layout,
		                              sampled_access(image),
		                              {view.base_level, view.level_count, view.base_layer, view.layer_count},
		                              metadata_of(m_draw_targets->depth.desc), view.base_layer, m_draw_targets->depth.desc.info,
		                              view});
	}
	draw_state.dynamic       = dynamic_state;
	draw_state.targets_epoch = textures.ImageSetEpoch();
	draw_state.used_frame    = m_context.FrameNumber();
	for (const auto& target: draw_state.targets) draw_state.target_ids.push_back(target.id);
	auto& slot               = cache.states[state_key];
	if (!slot) slot = std::make_unique<NativeXprDrawState>();
	const auto captures = slot->captures;
	*slot               = std::move(draw_state);
	// (Its emission and the image sets' transits under it were for the state before.)
	slot->captures   = captures + 1;
	static std::atomic<uint64_t> capture_ids {0};
	slot->capture_id = ++capture_ids;
	cache.last_state = nullptr; // (the state at this address is another)
	if (cache.emit.state == slot.get()) cache.emit.state = nullptr;
}

// The normal path drew a draw a try asked to store as no record can hold it (mesh, draw data,
// 8-bit indices): its shader pair stops asking.
void RenderExecutor::NativeXprRefuseStore(CommandBuffer& buffer) {
	auto& cache = NativeXprState();
	++cache.counters.refused;
	++cache.refuse_reasons["draw_shape"];
	std::vector<uint32_t> key;
	NativeXprKeyOf(buffer, key);
	cache.unstorable[NativeXprPairKey(key)] = true;
}

// Buffers and images: once per frame, or when the buffer or image set moved.
bool RenderExecutor::NativeXprValidateResources(NativeXprRecord& record, uint64_t frame, bool mapping_moved) {
	auto& buffers  = m_context.GetBufferCache();
	auto& textures = m_context.GetTextureCache();
	const auto cpu_epoch_now = [&] {
		uint64_t sum = 0;
		for (const auto& dep: record.buffers) {
			const auto epoch = buffers.CpuDirtyEpoch(dep.address, dep.size);
			if (epoch == 0) return uint64_t {0};
			sum += epoch;
		}
		return sum;
	};
	if (!mapping_moved && record.validated_frame == frame && record.buffer_set_epoch == buffers.BufferSetEpoch() &&
	    record.image_set_epoch == textures.ImageSetEpoch()) {
		const auto cpu_epoch     = cpu_epoch_now();
		const bool buffers_clean = record.buffers.empty() || (cpu_epoch != 0 && cpu_epoch == record.cpu_epoch) ||
		                           std::ranges::none_of(record.buffers, [&](const auto& dep) {
			                           return buffers.IsRegionCpuModified(dep.address, dep.size);
		                           });
		const auto image_epoch  = Image::CpuDirtyEpoch();
		bool       images_clean = image_epoch == record.image_dirty_epoch;
		if (buffers_clean && !images_clean) {
			images_clean = std::ranges::none_of(record.images, [&](const auto& dep) {
				const auto* image = textures.m_slot_images.try_get(dep.id);
				return image == nullptr || image->serial != dep.serial || TextureCache::RefreshPending(*image);
			});
			if (images_clean) record.image_dirty_epoch = image_epoch;
		}
		if (buffers_clean && images_clean) return true;
	}
	const auto image_epoch_at_proof = Image::CpuDirtyEpoch();
	const auto cpu_epoch            = cpu_epoch_now();
	const bool cpu_proven           = !mapping_moved && cpu_epoch != 0 && cpu_epoch == record.cpu_epoch;
	for (const auto& dep: record.buffers) {
		if (mapping_moved && m_context.UnmappedSince(record.mapping_version, dep.address, dep.size)) return false;
		if (!buffers.TouchLiveBuffer(dep.id, dep.handle, dep.address, dep.size)) return false;
		if (cpu_proven || !buffers.IsRegionCpuModified(dep.address, dep.size)) continue;
		// The guest rewrote the range: bring the owner up to date as the store did.
		if (buffers.IsRegionGpuModified(dep.address, dep.size)) return false;
		buffers.EnsureBufferContents(dep.address, dep.size);
		if (buffers.IsRegionCpuModified(dep.address, dep.size)) return false;
	}
	record.cpu_epoch        = cpu_epoch_now();
	record.buffer_set_epoch = buffers.BufferSetEpoch();
	const bool images_moved = textures.ImageSetEpoch() != record.image_set_epoch;
	if (images_moved) {
		// Texel buffers: still no image starting at one.
		for (const auto& dep: record.buffers)
			if (dep.texel && textures.HasImageStartingAt(dep.address)) return false;
	}
	for (const auto& dep: record.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial) return false;
		// The guest rewrote it (a texture streaming in): upload it as FindTexture does.
		if (TextureCache::RefreshPending(*image)) {
			if (!image->registered || image->depth_id || image->binding.needs_rebind) return false;
			(void)textures.FindTexture(dep.id, dep.desc);
			image = textures.m_slot_images.try_get(dep.id);
			if (image == nullptr || image->serial != dep.serial || TextureCache::RefreshPending(*image)) return false;
		}
		// An image still in the registration the proof saw keeps the backing and resources it checked (chenxiao07's
		// KYTY_NATIVE_IMAGE_PROOF): only images registered since are checked again, under the cache's lock.
		if (images_moved &&
		    !(NativeXprImageProofEnabled() && image->registered && image->registrations == dep.registration &&
		      !image->depth_id && !image->binding.needs_rebind) &&
		    !textures.IsSampledImageCurrent(dep.id, dep.desc))
			return false;
		(void)textures.GetImage(dep.id); // LRU, as FindTexture does
	}
	record.image_set_epoch   = textures.ImageSetEpoch();
	record.mapping_version   = m_context.MappedRangesVersion();
	record.validated_frame   = frame;
	record.image_dirty_epoch = image_epoch_at_proof;
	return true;
}

// Buffer descriptors whose base address moved (a ring of constants, a per-frame allocation): the
// slots are resolved again as FindBuffers and the store resolve them (a live, CPU-clean owner, no
// image starting at a texel range) and a new set is written with them. False when a slot does not
// resolve to what a record may hold.
bool RenderExecutor::NativeXprRebindBuffers(NativeXprRecord& record, std::span<const std::pair<uint32_t, uint32_t>> slots) {
	if (record.pipelines.empty() || record.set_writes.empty()) return false;
	auto&       buffers   = m_context.GetBufferCache();
	auto&       textures  = m_context.GetTextureCache();
	const auto& graphics  = m_context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	const auto  max_range = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	for (const auto& [st, r]: slots) {
		auto& stage = record.stages[st];
		if (r >= stage.program->info.buffers.size() || r >= record.snapshots[st].buffers.size() || record.buffers_start[st] == UINT32_MAX)
			return false;
		const auto& resource = stage.program->info.buffers[r];
		const auto  slot     = resource.descriptor_index;
		if (slot == UINT32_MAX || resource.written || resource.atomic) return false;
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(record.snapshots[st].buffers[r]);
		const auto address    = descriptor.Base48();
		auto       size       = descriptor.GetSize();
		if (address == 0 || size == 0 || descriptor.NumRecords() == UINT32_MAX) return false;
		size = Libs::LibKernel::Memory::ClampRangeSize(address, size);
		if (size == 0 || size > max_range) return false;
		const auto  id    = buffers.FindBuffer(address, size);
		const auto* owner = buffers.GetBufferIfLive(id);
		if (owner == nullptr || !owner->IsInBounds(address, size)) return false;
		if (buffers.IsRegionCpuModified(address, size)) {
			if (buffers.IsRegionGpuModified(address, size)) return false;
			buffers.EnsureBufferContents(address, size);
			if (buffers.IsRegionCpuModified(address, size)) return false;
		}
		const bool texel = resource.formatted || (resource.read && Config::SyncRawImageBuffersEnabled());
		if (texel && textures.HasImageStartingAt(address)) return false;
		const auto offset     = owner->Offset(address);
		const auto aligned    = offset - offset % alignment;
		const auto adjustment = offset - aligned;
		if (adjustment >= 256 || size > max_range - adjustment) return false;
		const auto position = record.buffers_start[st] + slot;
		if (position >= record.set_buffers.size()) return false;
		record.set_buffers[position] = {owner->Handle(), aligned, size + adjustment};
		auto&      word  = stage.shader_data[stage.program->bindings.memory_offset_dword + slot / 4u];
		const auto shift = (slot % 4u) * 8u;
		word             = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(adjustment) << shift);
		const NativeXprRecord::BufferDep dep {address, size, id, owner->Handle(), texel, st, r, slot};
		const auto found = std::ranges::find_if(record.buffers, [&](const auto& item) {
			return item.stage == st && item.resource == r;
		});
		if (found != record.buffers.end()) *found = dep;
		else record.buffers.push_back(dep);
	}
	// The new set is written with the rest of the payload too: none of it may be gone since the record was proven.
	for (const auto& dep: record.buffers) {
		const auto* owner = buffers.GetBufferIfLive(dep.id);
		if (owner == nullptr || owner->Handle() != dep.handle) return false;
	}
	for (const auto& dep: record.images) {
		const auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial) return false;
	}
	// The set may be in flight: a new one with the payload.
	NativeXprRetireSet(record.pool, record.set);
	record.set = NativeXprAllocateSet(record.pipelines.front().second->descriptor_set_layout, record.pool);
	if (!record.set) return false;
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(record.set_writes.size());
	for (const auto& slot: record.set_writes) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = record.set;
		write.dstBinding      = slot.binding;
		write.descriptorCount = slot.count;
		write.descriptorType  = slot.type;
		if (slot.image) write.pImageInfo = record.set_images.data() + slot.start;
		else write.pBufferInfo = record.set_buffers.data() + slot.start;
		writes.push_back(write);
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	// The new buffers are proven with the rest of the resources.
	record.validated_frame = UINT64_MAX;
	return true;
}

// At every use: is the record's content still what a fresh translation gives?
bool RenderExecutor::NativeXprValidate(NativeXprRecord& record, uint64_t frame, bool& broken) {
	auto&      cache         = *m_native_xpr;
	broken                   = false;
	const bool mapping_moved = m_context.MappedRangesVersion() != record.mapping_version;
	if (mapping_moved) {
		// An unmap can take words the record reads.
		bool unmapped = false;
		record.reads.ForEachRead([&](uint64_t address, uint64_t size) {
			unmapped = unmapped || m_context.UnmappedSince(record.mapping_version, address, size);
		});
		if (unmapped) return ++cache.counters.validate_unmapped, broken = true, false;
	}
	// The words the SRT evaluation read: any the log does not tolerate makes a new record (the log tolerates what a
	// draw's resolution ahead does: rewritten descriptor bases and statistics fields, flat constants read again).
	bool still_valid = false;
	{
		NativeXprSection section(cache.counters.sec_ns[1]);
		still_valid = record.reads.StillValid();
	}
	if (!still_valid) {
		if (NativeXprStatsEnabled()) {
			uint32_t index = 0, size = 0, old_value = 0, new_value = 0;
			uint64_t address = 0;
			if (record.reads.FirstMismatch(index, address, size, old_value, new_value)) {
				const auto pair = NativeXprPairKey(record.key);
				auto& item      = cache.mismatches[pair * 31u + index];
				if (item.count++ == 0 || (item.count & 1023u) == 0) {
					item.address = address; item.size = size; item.old_value = old_value; item.new_value = new_value;
					item.index = index; item.pair = pair;
				}
			}
		}
		return ++cache.counters.validate_unlearned, false;
	}
	NativeXprSection validate_section(cache.counters.sec_ns[0]);
	std::array<std::array<uint32_t, 2>, 64> bases_before {};
	std::array<uint32_t, 2>                 bases_count {};
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		const auto& buffers_of = record.snapshots[st].buffers;
		bases_count[st]        = static_cast<uint32_t>(std::min<size_t>(buffers_of.size(), bases_before.size()));
		for (uint32_t r = 0; r < bases_count[st]; ++r)
			bases_before[r][st] = buffers_of[r].dwords[0] ^ (buffers_of[r].dwords[1] & 0xffffu);
	}
	record.reads.RefreshDescriptorWords(std::span(record.snapshots).first(record.stage_count),
	                                    std::span<const ShaderRecompiler::IR::ResourcePlan* const>(record.plans).first(record.stage_count));
	record.reads.RefreshFlatLoads(std::span(record.snapshots).first(record.stage_count), false);
	auto& rebind = cache.rebind_buffers;
	rebind.clear();
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		const auto& buffers_of = record.snapshots[st].buffers;
		for (uint32_t r = 0; r < bases_count[st]; ++r)
			if (bases_before[r][st] != (buffers_of[r].dwords[0] ^ (buffers_of[r].dwords[1] & 0xffffu))) rebind.emplace_back(st, r);
	}
	if (!rebind.empty()) {
		++cache.counters.rebinds;
		if (!NativeXprRebindBuffers(record, rebind)) return ++cache.counters.validate_resources, broken = true, false;
	}
	if (!NativeXprValidateResources(record, frame, mapping_moved))
		return ++cache.counters.validate_resources, broken = true, false;
	return true;
}

// The targets in the state's layouts. What the normal path's acquisition establishes must still
// hold: the image is the same registered object, GPU owned (so CPU reads download it) and CPU
// clean, and no fast clear is pending in its metadata.
bool RenderExecutor::NativeXprEmitTargets(const NativeXprDrawState& draw_state, CommandRecorder vk_buffer,
                                          const TableImageSet* reading) {
	auto& textures = m_context.GetTextureCache();
	for (const auto& target: draw_state.targets) {
		auto* image = textures.m_slot_images.try_get(target.id);
		if (image == nullptr || image->serial != target.serial || !image->registered || image->binding.needs_rebind ||
		    !image->SafeToDownload())
			return false;
		// (Surface metadata holds a depth target's HTile clears here: a color target's are in its metadata's bytes.)
		if (target.metadata != 0 && target.requested.IsDepth() && textures.IsMetaCleared(target.metadata, target.slice))
			return false;
		// A color target's fast clear in its metadata (the guest's keys, or a register clear only the color buffer
		// decodes) is applied by the normal path's lookup (TextureCache::MaterializeColorClear): Demon's Souls clears
		// a DCC target at Boletaria once a frame, and ~200 table draws a frame drew the character's rim light into it
		// over the last frame's contents.
		if (!target.requested.IsDepth() &&
		    (target.requested.metadata.kind == ImageMetadataKind::Dcc ||
		     target.requested.metadata.kind == ImageMetadataKind::Cmask) &&
		    !textures.ColorClearSettled({target.requested, target.view, TextureCache::BindingType::RenderTarget},
		                                target.clear_memo))
			return false;
	}
	for (const auto& target: draw_state.targets) {
		auto& image = textures.GetImage(target.id); // LRU, as acquisition does
		// (A target the draw's image set samples: with the shader access added, as CommitBindings transits it.)
		image.Transit(target.layout, reading != nullptr ? target.access | TableTargetRead(*reading, target.id) : target.access,
		              target.range, vk_buffer);
	}
	return true;
}

// A draw state's dynamic state, only what differs from what the emissions recorded in the command buffer (the
// scheduler's DynamicState marks the differences: Assign, Commit).
void RenderExecutor::NativeXprEmitDynamic(NativeXprCache&, CommandRecorder vk_buffer,
                                          const NativeXprDrawState& draw_state, bool) {
	auto& dynamic = m_context.GetCommandScheduler().GetDynamicState();
	dynamic.Assign(draw_state.dynamic);
	dynamic.Commit(m_context.GetGraphics(), vk_buffer);
}

// A deleted target image took the state's attachment views along, and an image registered over
// a target may be what the registers now select: the normal path captures the state again.
bool RenderExecutor::NativeXprTargetsCurrent(const NativeXprDrawState& draw_state) {
	auto&      textures = m_context.GetTextureCache();
	const auto current  = textures.ImageSetEpoch();
	if (draw_state.targets_epoch == current) return true;
	for (const auto& target: draw_state.targets) {
		const auto* image = textures.m_slot_images.try_get(target.id);
		if (image == nullptr || image->serial != target.serial || !image->registered)
			return ++m_native_xpr->counters.target_object, false;
	}
	// A lookup of a target's range finds the same image while no image over that range was
	// registered or unregistered since the targets were proven (the lookups only see the range).
	bool       moved = false;
	const bool known = textures.ForEachImageSetChangeSince(draw_state.targets_epoch, [&](uint64_t address, uint64_t size) {
		for (const auto& target: draw_state.targets)
			moved = moved || ImageRangeOverlaps(address, size, target.requested.data.address, target.requested.data.size);
	});
	if (!known || moved) {
		// Images were registered or unregistered over a target's range: a lookup still finds the
		// target when it is the last image with the requested backing there.
		for (const auto& target: draw_state.targets)
			if (!textures.IsTargetCurrent(target.id, target.requested)) return ++m_native_xpr->counters.target_lookup, false;
	}
	draw_state.targets_epoch = current;
	return true;
}

bool RenderExecutor::NativeXprEmit(CommandBuffer& buffer, NativeXprRecord& record, NativeXprDrawState& draw_state,
                                   const PipelineCache::Pipeline& pipeline, uint64_t args_addr, uint64_t index_base,
                                   uint64_t index_bytes, vk::IndexType index_type) {
	auto& buffer_cache = m_context.GetBufferCache();
	auto& stream       = buffer_cache.GetUtilityBuffer(MemoryUsage::Stream);
	if (stream.Handle() != record.upload) return false;
	// Constants into the stream buffer, at the set's dynamic offsets (binding order).
	const auto& limits    = m_context.GetGraphics().GetPhysicalDeviceProperties().limits;
	const auto  alignment = std::max(std::max<uint64_t>(256, limits.minUniformBufferOffsetAlignment),
	                                 uint64_t {limits.minStorageBufferOffsetAlignment});
	std::array<uint32_t, 8>        offsets {};
	ShaderRecompiler::IR::PushData push_data;
	if (record.dynamic_bindings.size() > offsets.size()) return false;
	auto& sec = m_native_xpr->counters.sec_ns;
	std::optional<NativeXprSection> section;
	section.emplace(sec[2]);
	for (size_t i = 0; i < record.dynamic_bindings.size(); ++i) {
		const auto& binding = record.dynamic_bindings[i];
		const auto& stage   = record.stages[binding.stage];
		const auto& values  = binding.srt ? record.snapshots[binding.stage].flattened_srt : stage.shader_data;
		offsets[i]          = static_cast<uint32_t>(
            buffer_cache.StreamUpload(values.data(), values.size() * sizeof(uint32_t), alignment));
	}
	for (uint32_t i = 0; i < record.stage_count; ++i) {
		const auto& stage = record.stages[i];
		if (stage.push_start != ShaderRecompiler::IR::PushData::NoStart) {
			if (stage.push_start + stage.shader_data.size() > push_data.dwords.size()) return false;
			std::ranges::copy(stage.shader_data, push_data.dwords.begin() + stage.push_start);
		}
	}
	// Indices and arguments are read by the GPU; lookups may restart the scheduler, so the command
	// buffer handle is taken after them.
	section.emplace(sec[3]);
	if (index_bytes <= BufferCache::CACHING_PAGESIZE && buffer_cache.IsRegionCpuModified(index_base, index_bytes) &&
	    !buffer_cache.IsRegionGpuModified(index_base, index_bytes))
		buffer_cache.EnsureBufferContents(index_base, index_bytes);
	const auto* direct                 = m_native_xpr_direct;
	const auto [indices, index_offset] = buffer_cache.ObtainBuffer(index_base, index_bytes, false);
	const auto [arguments, args_offset] =
	    direct != nullptr ? std::pair<Buffer*, uint64_t> {nullptr, 0}
	                      : buffer_cache.ObtainBuffer(args_addr, sizeof(vk::DrawIndexedIndirectCommand), false);
	if (indices == nullptr || (direct == nullptr && arguments == nullptr) || buffer.IsInvalid()) return false;
	// Vertex buffers as the normal path acquires them, from the draw's resolution.
	PreparedVertexBuffers vertex_bindings;
	if (record.vertex_buffers) {
		auto* resolved = m_resolved_draw;
		if (resolved == nullptr || (!m_native_xpr->resolution_valid && !resolved->programs.reads.StillValid()))
			return false;
		const auto& vertex = resolved->storage.state.vertex_info[0];
		if (vertex.buffers_num <= 0 || resolved->storage.state.vertex_info[0].stage.module->id != record.vertex_program) return false;
		AcquireVertexBuffers(buffer, vertex, vertex_bindings);
		if (buffer.IsInvalid()) return false;
	}
	auto vk_buffer = buffer.Handle();

	auto& textures = m_context.GetTextureCache();
	section.emplace(sec[4]);
	if (!NativeXprEmitTargets(draw_state, vk_buffer)) return false;
	section.emplace(sec[5]);
	for (const auto& dep: record.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr) return false;
		image->Transit(dep.layout, vk::AccessFlagBits2::eShaderRead, dep.range, vk_buffer);
	}
	section.emplace(sec[6]);
	auto& scheduler = m_context.GetCommandScheduler();
	auto& dynamic   = scheduler.GetDynamicState();
	dynamic.Assign(draw_state.dynamic);
	dynamic.Commit(m_context.GetGraphics(), vk_buffer);
	buffer.BeginRendering(draw_state.rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	vk_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline_layout, 0, 1, &record.set,
	                             static_cast<uint32_t>(record.dynamic_bindings.size()), offsets.data());
	if (record.has_push)
		vk_buffer.pushConstants(pipeline.pipeline_layout, record.push_stages, 0, sizeof(push_data),
		                        push_data.dwords.data());
	CommitVertexBuffers(vk_buffer, vertex_bindings);
	vk_buffer.bindIndexBuffer(indices->Handle(), index_offset, index_type);
	dynamic.MoveGeneration(); // (its pipeline, descriptors and index buffer replace a table draw's)
	if (direct != nullptr) {
		// The offsets as DrawIndex() resolves them (ResolveDrawOffsets()).
		auto     vertex_offset   = static_cast<int32_t>(buffer.GetUserConfig().GetIndexOffset());
		uint32_t instance_offset = 0;
		if (record.fetch_embedded) {
			const auto& program   = *record.stages[0].program;
			const auto& user_data = record.snapshots[0].user_data;
			const auto  base      = static_cast<int32_t>(program.user_data_base);
			if (vertex_offset == 0 && program.info.vertex_offset_sgpr >= base) {
				const auto index = static_cast<uint32_t>(program.info.vertex_offset_sgpr - base);
				if (index < user_data.size()) vertex_offset = static_cast<int32_t>(user_data[index]);
			}
			if (program.info.instance_offset_sgpr >= base) {
				const auto index = static_cast<uint32_t>(program.info.instance_offset_sgpr - base);
				if (index < user_data.size()) instance_offset = user_data[index];
			}
		}
		vk_buffer.drawIndexed(direct->index_count, direct->instance_count, 0, vertex_offset + direct->base_vertex,
		                      instance_offset);
	} else {
		vk_buffer.drawIndexedIndirect(arguments->Handle(), args_offset, 1, sizeof(vk::DrawIndexedIndirectCommand));
	}
	draw_state.used_frame = m_context.FrameNumber();
	return true;
}

// A relocated record leaves its maps and service.
void RenderExecutor::NativeXprDropRelocated(NativeXprRecord& record) {
	auto& cache = *m_native_xpr;
	if (const auto shape = cache.relocated_shapes.find(record.shape_hash);
	    shape != cache.relocated_shapes.end() && shape->second == &record)
		cache.relocated_shapes.erase(shape);
	const auto found = cache.relocated.find(record.full_hash);
	if (found != cache.relocated.end() && found->second.get() == &record) {
		auto owned = std::move(found->second);
		cache.relocated.erase(found);
		NativeXprRetire(std::move(owned));
	}
}

// The key missed: the relocated record of the draw's resolution ahead, by its programs and
// descriptors (see the top of this file), with the resolution's constants. Null when there is
// none; then `store` when one should be stored (the same descriptors came again).
RenderExecutor::NativeXprRecord* RenderExecutor::NativeXprRelocated(uint64_t frame, bool& store) {
	auto& cache = *m_native_xpr;
	store       = false;
	auto* resolved = m_resolved_draw;
	if (resolved == nullptr || !resolved->programs.reads.StillValid()) {
		++cache.counters.relocated_unresolved;
		return nullptr;
	}
	cache.resolution_valid = true;
	const auto& state = resolved->storage.state;
	if (state.VertexStages().size() != 1) return nullptr;
	const std::array<const ShaderStageRuntime*, 2> runtimes {&state.vertex_info[0].stage, &state.ps_input_info.stage};
	const uint32_t                                 stage_count = state.ps_active ? 2u : 1u;
	std::array<NativeXprStageView, 2>              views {};
	for (uint32_t st = 0; st < stage_count; ++st) {
		const auto& runtime = *runtimes[st];
		if (!runtime) return nullptr;
		const auto& snapshot = *runtime.resources;
		if (!snapshot.image_tables.empty()) return nullptr;
		views[st] = {&runtime.module->info, &snapshot.buffers, &snapshot.images, &snapshot.samplers};
	}
	const auto vertex_program = state.vertex_info[0].stage.module->id;
	const auto pixel_program  = state.ps_active ? state.ps_input_info.stage.module->id : 0;
	const auto [full, shape] =
	    NativeXprRelocationHashes(vertex_program, pixel_program, std::span {views.data(), stage_count});
	const auto matches = [&](const NativeXprRecord& record, bool same_bases) {
		if (record.stage_count != stage_count || record.vertex_program != vertex_program ||
		    record.pixel_program != pixel_program)
			return false;
		for (uint32_t st = 0; st < stage_count; ++st) {
			const auto& stage    = record.stages[st];
			const auto& snapshot = *runtimes[st]->resources;
			const auto& held = record.snapshots[st];
			if (stage.program != &runtimes[st]->module->info || held.samplers != snapshot.samplers ||
			    !NativeXprSameImagesButLodStats(held.images, snapshot.images) ||
			    held.buffers.size() != snapshot.buffers.size() || held.flattened_srt.size() != snapshot.flattened_srt.size() ||
			    held.user_data.size() != snapshot.user_data.size())
				return false;
			for (size_t i = 0; i < held.buffers.size(); ++i) {
				const auto& a = held.buffers[i];
				const auto& b = snapshot.buffers[i];
				if (a.dword_count != b.dword_count) return false;
				for (uint32_t w = 0; w < 8; ++w) {
					const auto mask = same_bases ? ~0u : NativeXprBufferShapeMask[w];
					if ((a.dwords[w] & mask) != (b.dwords[w] & mask)) return false;
				}
			}
		}
		return true;
	};
	NativeXprRecord* record = nullptr;
	bool             moved  = false;
	if (const auto found = cache.relocated.find(full); found != cache.relocated.end() && matches(*found->second, true)) {
		record = found->second.get();
	} else if (const auto similar = cache.relocated_shapes.find(shape);
	           similar != cache.relocated_shapes.end() && matches(*similar->second, false)) {
		record = similar->second;
		moved  = true;
	}
	const auto ask_store = [&] {
		if (const auto refused = cache.refused.find(full);
		    refused != cache.refused.end() && refused->second + NativeXprCache::RefuseFrames > frame)
			return;
		store                 = true;
		cache.relocated_full  = full;
		cache.relocated_shape = shape;
	};
	if (record == nullptr) {
		++cache.counters.relocated_miss;
		// (By shape: buffer bases that move every frame are rebound.)
		if (NativeXprCache::SeenAgainIn(cache.relocated_seen, shape, frame)) ask_store();
		return nullptr;
	}
	// The record of the shape already drew another instance this frame: this one gets a record of its own.
	if (moved && record->used_frame == frame && NativeXprInstancesEnabled()) {
		++cache.counters.relocated_instances;
		ask_store();
		return nullptr;
	}
	// The resolution's constants: the flattened SRT and the user data part of the shader data (the
	// memory offsets after it are the record's buffers').
	auto& rebind = cache.rebind_buffers;
	rebind.clear();
	for (uint32_t st = 0; st < stage_count; ++st) {
		auto&       stage    = record->stages[st];
		const auto& program  = *stage.program;
		const auto& snapshot = *runtimes[st]->resources;
		const auto& regs     = program.info.user_data_registers;
		if (regs.size() > stage.shader_data.size()) {
			NativeXprDropRelocated(*record);
			ask_store();
			return nullptr;
		}
		auto& held = record->snapshots[st];
		std::ranges::copy(snapshot.flattened_srt, held.flattened_srt.begin());
		std::ranges::copy(snapshot.user_data, held.user_data.begin());
		for (size_t i = 0; i < regs.size(); ++i) stage.shader_data[i] = snapshot.user_data[regs[i] - program.user_data_base];
		std::ranges::copy(snapshot.images, held.images.begin());
		if (moved) {
			for (uint32_t i = 0; i < held.buffers.size(); ++i) {
				if (held.buffers[i] == snapshot.buffers[i]) continue;
				held.buffers[i] = snapshot.buffers[i];
				rebind.emplace_back(st, i);
			}
		}
	}
	if (moved) {
		++cache.counters.relocated_rebinds;
		if (!rebind.empty() && !NativeXprRebindBuffers(*record, rebind)) {
			++cache.counters.relocated_broken;
			NativeXprDropRelocated(*record);
			ask_store();
			return nullptr;
		}
		// Now found by its new descriptors.
		auto owner = cache.relocated.find(record->full_hash);
		EXIT_IF(owner == cache.relocated.end() || owner->second.get() != record);
		auto owned = std::move(owner->second);
		cache.relocated.erase(owner);
		if (const auto other = cache.relocated.find(full); other != cache.relocated.end())
			NativeXprDropRelocated(*other->second);
		record->full_hash  = full;
		record->shape_hash = shape;
		cache.relocated.emplace(full, std::move(owned));
		cache.relocated_shapes[shape] = record;
	}
	const bool mapping_moved = m_context.MappedRangesVersion() != record->mapping_version;
	if (!NativeXprValidateResources(*record, frame, mapping_moved)) {
		++cache.counters.relocated_broken;
		NativeXprDropRelocated(*record);
		ask_store();
		return nullptr;
	}
	++cache.counters.relocated_hits;
	return record;
}

// A direct indexed draw of one object: as an indirect one (NativeXprTry()), drawn with its counts.
bool RenderExecutor::NativeXprTryDirect(CommandBuffer& buffer, const DrawIndexArgs& args) {
	if (!NativeXprEnabled() || !NativeXprDirectEnabled() || args.indirect.address != 0 || args.offset_source != DrawOffsetSource::DrawState ||
	    args.index_addr == nullptr || args.index_count == 0 || args.instance_count == 0 ||
	    args.first_instance != 0 || args.index_type_and_size > 1)
		return false;
	const auto index_bytes = uint64_t {args.index_count} * (args.index_type_and_size == 0 ? 2u : 4u);
	m_native_xpr_direct    = &args;
	const bool drawn = NativeXprTry(buffer, 0, reinterpret_cast<uint64_t>(args.index_addr), index_bytes,
	                                args.index_type_and_size == 0 ? vk::IndexType::eUint16 : vk::IndexType::eUint32);
	m_native_xpr_direct = nullptr;
	return drawn;
}

// An indexed indirect draw of one object, its arguments read by the host GPU.
bool RenderExecutor::NativeXprLikely(const HW::Shader& shaders) const noexcept {
	if (!NativeXprEnabled() || !NativeXprResolveSkipEnabled()) return false;
	// The object's own key: its last try drew it from a record whose key it is (a draw of the pair whose
	// object is new or moved still needs its resolution).
	static thread_local std::vector<uint32_t> key;
	NativeXprKeyOfShaders(shaders, key);
	const auto tag = NativeXprHash(key) | 1ull;
	return m_native_xpr_drawn[tag & (m_native_xpr_drawn.size() - 1)].load(std::memory_order_relaxed) == tag;
}

bool RenderExecutor::NativeXprTry(CommandBuffer& buffer, uint64_t args_addr, uint64_t index_base,
                                  uint64_t index_bytes, vk::IndexType index_type) {
	// (A table draw's store or pending variant: the normal path draws, storing no record.)
	if (!NativeXprEnabled() || buffer.IsInvalid()) return false;
	const auto pair = NativeXprPairOf(buffer);
	if (index_base == 0 || index_bytes == 0) return false;
	auto& cache = NativeXprState();
	bool  timed_hit = false; // DSP-INSTR
	struct TryTimer {
		NativeXprCache&                       cache;
		const bool&                           hit;
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		~TryTimer() {
			const auto ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			(hit ? cache.counters.ns_hit : cache.counters.ns_try_miss) += ns;
			++(hit ? cache.counters.n_hit : cache.counters.n_try_miss);
		}
	} try_timer {cache, timed_hit};
	std::optional<NativeXprSection> pre;
	pre.emplace(cache.counters.sec_ns[8]);
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());
	NativeXprCollect();
	const auto& ucfg = buffer.GetUserConfig();
	const auto& ctx  = buffer.GetRegisters();
	// What a record holds: an indexed triangle list of a vertex and a pixel stage that fetch their
	// own vertices, with no draw data pushed (object id) and no metadata color operation (color
	// writes off or normal; the store refuses tessellation).
	if (ucfg.GetPrimType() != Prospero::PrimitiveType::kTriList || ctx.HasObjectPrimitiveIdOutput() ||
	    ctx.GetColorControl().mode > 1u || (ucfg.GetPrimitiveResetControl() & 0x1u) != 0)
		return false;
	const auto frame = m_context.FrameNumber();
	auto&      use   = cache.pairs[pair];
	if (use.resting_until > frame || cache.unstorable.contains(pair)) return false;
	if (++use.tries > NativeXprCache::PairTriesWithoutRecord && use.stored == 0) {
		use.tries         = 0;
		use.resting_until = frame + NativeXprCache::PairRestFrames;
		return false;
	}
	++cache.counters.tries;
	pre.emplace(cache.counters.sec_ns[7]);
	cache.state_key = NativeXprStateKeyOf(buffer, &m_state_key_cache, m_context_id, m_user_config_id);
	NativeXprKeyOf(buffer, cache.key);
	const auto hash  = NativeXprHash(cache.key);
	cache.tried_hash = hash;
	pre.reset();
	// The resolve thread skips the resolution ahead of draws of objects whose last try drew them from their record.
	bool drawn = false;
	struct DrawnHint {
		std::atomic<uint64_t>& slot;
		const uint64_t         tag;
		const bool&            drawn;
		~DrawnHint() { slot.store(drawn ? tag : 0, std::memory_order_relaxed); }
	} hint {m_native_xpr_drawn[(hash | 1ull) & (m_native_xpr_drawn.size() - 1)], hash | 1ull, drawn};
	const auto request_store = [&] {
		if (const auto budget = NativeXprStoreBudget(); budget != 0u) {
			if (cache.store_frame != frame) {
				cache.store_frame    = frame;
				cache.store_requests = 0;
			}
			if (cache.store_requests >= budget) {
				++cache.counters.budget_denied; // asked again by a later draw of the object
				return;
			}
			++cache.store_requests;
		}
		m_native_xpr_store = true;
	};
	cache.store_relocated    = false;
	cache.resolution_valid   = false;
	const auto found = cache.records.find(hash);
	NativeXprRecord* chosen = nullptr;
	if (found == cache.records.end() || found->second.empty() || found->second.front()->key != cache.key) {
		++cache.counters.miss_key;
		if (const auto refused = cache.refused.find(hash);
		    refused != cache.refused.end() && refused->second + NativeXprCache::RefuseFrames > frame)
			return false;
		// A key that comes again gets a record of its own; otherwise the user data move (see the top
		// of this file).
		if (cache.SeenAgain(hash, frame)) {
			request_store();
			return false;
		}
		bool store_relocated = false;
		chosen               = NativeXprRelocateEnabled() ? NativeXprRelocated(frame, store_relocated) : nullptr;
		if (chosen == nullptr) {
			if (store_relocated) {
				cache.store_relocated = true;
				request_store();
			}
			return false;
		}
		cache.store_relocated = true;
		cache.relocated_full  = chosen->full_hash;
		cache.relocated_shape = chosen->shape_hash;
	}
	// The variant whose words and resources still hold (the last one used first).
	auto* const variants_of = chosen == nullptr ? &found->second : nullptr;
	for (size_t i = 0; variants_of != nullptr && i < variants_of->size();) {
		auto& variants = *variants_of;
		bool broken = false;
		if (NativeXprValidate(*variants[i], frame, broken)) {
			chosen = variants[i].get();
			if (i != 0)
				std::rotate(variants.begin(), variants.begin() + static_cast<std::ptrdiff_t>(i),
				            variants.begin() + static_cast<std::ptrdiff_t>(i) + 1);
			break;
		}
		if (broken) {
			NativeXprRetire(std::move(variants[i]));
			variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}
		++i;
	}
	if (chosen == nullptr) {
		++cache.counters.miss_validate;
		if (found->second.empty()) cache.records.erase(found);
		request_store();
		return false;
	}
	pre.emplace(cache.counters.sec_ns[9]);
	auto& record      = *chosen;
	record.used_frame = frame;
	++record.uses;
	// Learn the order objects are drawn in, and prefetch the record the next draw is likely to use (chenxiao07's
	// KYTY_NATIVE_XPR_PREDICT). A wrong guess only wastes the prefetch.
	if (NativeXprPredictEnabled()) {
		if (cache.last_hash != 0 && cache.last_hash != hash) {
			if (const auto previous = cache.records.find(cache.last_hash);
			    previous != cache.records.end() && !previous->second.empty())
				previous->second.front()->next_hash = hash;
		}
		cache.last_hash = hash;
		if (record.next_hash != 0) {
			if (const auto next = cache.records.find(record.next_hash);
			    next != cache.records.end() && !next->second.empty())
				NativeXprPrefetch(*next->second.front());
		}
	}
	const auto state_it = cache.states.find(cache.state_key);
	const auto pipeline_it =
	    std::ranges::find(record.pipelines, cache.state_key, &std::pair<uint64_t, const PipelineCache::Pipeline*>::first);
	if (state_it == cache.states.end() || pipeline_it == record.pipelines.end()) {
		++cache.counters.miss_state;
		request_store();
		return false;
	}
	auto& draw_state = *state_it->second;
	if (!NativeXprTargetsCurrent(draw_state)) {
		++cache.counters.miss_target;
		request_store();
		return false;
	}
	// A sampled image that is also a render target takes the normal path.
	for (const auto& dep: record.images)
		for (const auto& target: draw_state.targets)
			if (dep.id == target.id) {
				++cache.counters.miss_target;
				return false;
			}
	pre.reset();
	if (!NativeXprEmit(buffer, record, draw_state, *pipeline_it->second, args_addr, index_base, index_bytes,
	                   index_type)) {
		++cache.counters.miss_emit;
		return false;
	}
	++cache.counters.hits;
	drawn     = !record.relocated;
	timed_hit = true;
	return true;
}
