#include "graphics/capture/gpuTrace.h"

#include "common/file.h"
#include "common/outputReservation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <xxhash.h>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Libs::Graphics::Capture {
namespace {

constexpr std::array<uint8_t, 8> Magic {'K', 'Y', 'C', 'A', 'P', '\r', '\n', 0x1a};
constexpr std::array<uint8_t, 8> FooterMagic {'K', 'Y', 'C', 'E', 'N', 'D', '\r', '\n'};
constexpr uint32_t HeaderSize       = 24;
constexpr uint32_t RecordHeaderSize = 40;
constexpr uint32_t FooterSize       = 32;
constexpr uint32_t InterruptFlag    = 1u << 0u;
constexpr uint32_t NoQueue          = UINT32_MAX;

void Put16(std::span<uint8_t> dst, size_t offset, uint16_t value) {
	dst[offset]     = static_cast<uint8_t>(value);
	dst[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

void Put32(std::span<uint8_t> dst, size_t offset, uint32_t value) {
	for (uint32_t i = 0; i < 4; i++) {
		dst[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
	}
}

void Put64(std::span<uint8_t> dst, size_t offset, uint64_t value) {
	for (uint32_t i = 0; i < 8; i++) {
		dst[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
	}
}

uint16_t Get16(std::span<const uint8_t> src, size_t offset) {
	return static_cast<uint16_t>(src[offset]) |
	       static_cast<uint16_t>(src[offset + 1]) << 8u;
}

uint32_t Get32(std::span<const uint8_t> src, size_t offset) {
	uint32_t value = 0;
	for (uint32_t i = 0; i < 4; i++) {
		value |= static_cast<uint32_t>(src[offset + i]) << (i * 8u);
	}
	return value;
}

uint64_t Get64(std::span<const uint8_t> src, size_t offset) {
	uint64_t value = 0;
	for (uint32_t i = 0; i < 8; i++) {
		value |= static_cast<uint64_t>(src[offset + i]) << (i * 8u);
	}
	return value;
}

bool WriteExact(Common::File& file, const void* data, uint64_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size != 0) {
		const auto chunk = static_cast<uint32_t>(
		    std::min<uint64_t>(size, std::numeric_limits<uint32_t>::max()));
		uint32_t written = 0;
		file.Write(bytes, chunk, &written);
		if (written != chunk) {
			return false;
		}
		bytes += chunk;
		size -= chunk;
	}
	return true;
}

bool ReadExact(Common::File& file, void* data, uint64_t size) {
	auto* bytes = static_cast<uint8_t*>(data);
	while (size != 0) {
		const auto chunk = static_cast<uint32_t>(
		    std::min<uint64_t>(size, std::numeric_limits<uint32_t>::max()));
		uint32_t read = 0;
		file.Read(bytes, chunk, &read);
		if (read != chunk) {
			return false;
		}
		bytes += chunk;
		size -= chunk;
	}
	return true;
}

bool RecordHash(std::span<const uint8_t> header, std::span<const uint8_t> payload,
	            uint64_t& hash) {
	if (header.size() != RecordHeaderSize) {
		return false;
	}
	auto* state = XXH3_createState();
	if (state == nullptr) {
		return false;
	}
	auto header_copy = std::array<uint8_t, RecordHeaderSize> {};
	std::copy(header.begin(), header.end(), header_copy.begin());
	std::fill(header_copy.begin() + 32, header_copy.begin() + 40, 0);
	const bool success = XXH3_64bits_reset(state) != XXH_ERROR &&
	                     XXH3_64bits_update(state, header_copy.data(), header_copy.size()) !=
	                         XXH_ERROR &&
	                     XXH3_64bits_update(state, payload.data(), payload.size()) != XXH_ERROR;
	if (success) {
		hash = XXH3_64bits_digest(state);
	}
	XXH3_freeState(state);
	return success;
}

bool IsKnownType(TraceEventType type) {
	switch (type) {
		case TraceEventType::GraphicsSubmit:
		case TraceEventType::ComputeSubmit:
		case TraceEventType::FlipPreparation:
		case TraceEventType::SuspendRequest: return true;
	}
	return false;
}

bool SyncParentDirectory(const std::filesystem::path& path) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	(void)path;
	return true;
#else
	const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
	                                               : path.parent_path();
	const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
	if (directory < 0) {
		return false;
	}
	const bool synced = fsync(directory) == 0;
	close(directory);
	return synced;
#endif
}

bool ValidateEventShape(TraceEventType type, uint32_t queue, bool trigger_interrupt,
	                    size_t primary_size, size_t secondary_size, std::string& error) {
	switch (type) {
		case TraceEventType::GraphicsSubmit:
			if (queue != 0 || primary_size == 0) {
				error = "invalid graphics submission";
				return false;
			}
			return true;
		case TraceEventType::ComputeSubmit:
			if (queue < 0x20 || queue >= 0x58 || primary_size == 0 || secondary_size != 0) {
				error = "invalid compute submission";
				return false;
			}
			return true;
		case TraceEventType::FlipPreparation:
		case TraceEventType::SuspendRequest:
			if (queue != NoQueue || trigger_interrupt || primary_size != 0 || secondary_size != 0) {
				error = "invalid marker event";
				return false;
			}
			return true;
	}
	error = "unknown trace event";
	return false;
}

} // namespace

struct TraceWriter::Private {
	Common::File file;
	std::unique_ptr<Common::OutputReservation> reservation;
	std::mutex   mutex;
	uint64_t     sequence = 0;
	uint64_t     command_dwords = 0;
	bool         open     = false;
	bool         finalized = false;
};

TraceWriter::TraceWriter(std::filesystem::path path)
    : m_path(std::move(path)), m_private(std::make_unique<Private>()) {}

TraceWriter::~TraceWriter() {
	std::string ignored;
	if (!Finalize(ignored)) {
		CloseFile();
	}
}

std::unique_ptr<TraceWriter> TraceWriter::Create(const std::filesystem::path& path,
	                                              std::string& error) {
	error.clear();
	if (path.empty()) {
		error = "capture path is empty";
		return {};
	}
	auto writer = std::unique_ptr<TraceWriter>(new TraceWriter(path));
	if (!writer->Open(error)) {
		return {};
	}
	return writer;
}

bool TraceWriter::Open(std::string& error) {
	if (!m_path.parent_path().empty() &&
	    !Common::File::CreateDirectories(m_path.parent_path())) {
		error = "cannot create capture directory";
		return false;
	}
	m_private->reservation = Common::OutputReservation::Acquire(m_path, error);
	if (m_private->reservation == nullptr) {
		return false;
	}
	if (Common::File::IsFileExisting(m_path)) {
		error = "refusing to overwrite an existing capture file";
		return false;
	}
	if (!m_private->file.Create(m_path)) {
		error = "cannot create capture file";
		return false;
	}
	std::array<uint8_t, HeaderSize> header {};
	std::copy(Magic.begin(), Magic.end(), header.begin());
	Put16(header, 8, TraceMajorVersion);
	Put16(header, 10, TraceMinorVersion);
	Put32(header, 12, HeaderSize);
	Put32(header, 16, static_cast<uint32_t>(TraceCapability::CommandsOnly));
	if (!WriteExact(m_private->file, header.data(), header.size()) || !m_private->file.Flush()) {
		m_private->file.Close();
		error = "cannot write capture header";
		return false;
	}
	m_private->open = true;
	return true;
}

bool TraceWriter::RecordGraphics(std::span<const uint32_t> draw,
	                              std::span<const uint32_t> constants,
	                              bool trigger_interrupt_on_done, std::string& error) {
	return Record(TraceEventType::GraphicsSubmit, 0,
	              trigger_interrupt_on_done ? InterruptFlag : 0, draw, constants, error);
}

bool TraceWriter::RecordCompute(uint32_t queue, std::span<const uint32_t> commands,
	                             bool trigger_interrupt_on_done, std::string& error) {
	return Record(TraceEventType::ComputeSubmit, queue,
	              trigger_interrupt_on_done ? InterruptFlag : 0, commands, {}, error);
}

bool TraceWriter::RecordMarker(TraceEventType type, std::string& error) {
	return Record(type, NoQueue, 0, {}, {}, error);
}

bool TraceWriter::Record(TraceEventType type, uint32_t queue, uint32_t flags,
	                      std::span<const uint32_t> primary,
	                      std::span<const uint32_t> secondary, std::string& error) {
	error.clear();
	if (!IsKnownType(type) ||
	    !ValidateEventShape(type, queue, (flags & InterruptFlag) != 0, primary.size(),
	                        secondary.size(), error)) {
		return false;
	}
	if (primary.size() > UINT32_MAX || secondary.size() > UINT32_MAX ||
	    primary.size() + secondary.size() > (UINT32_MAX - RecordHeaderSize) / sizeof(uint32_t)) {
		error = "capture command payload is too large";
		return false;
	}

	std::lock_guard lock(m_private->mutex);
	if (!m_private->open) {
		error = "capture file is closed";
		return false;
	}
	const auto payload_size = (primary.size() + secondary.size()) * sizeof(uint32_t);
	std::vector<uint8_t> record(RecordHeaderSize + payload_size);
	Put32(record, 0, static_cast<uint32_t>(record.size()));
	Put32(record, 4, static_cast<uint32_t>(type));
	Put64(record, 8, ++m_private->sequence);
	Put32(record, 16, queue);
	Put32(record, 20, flags);
	Put32(record, 24, static_cast<uint32_t>(primary.size()));
	Put32(record, 28, static_cast<uint32_t>(secondary.size()));
	size_t offset = RecordHeaderSize;
	for (const auto word: primary) {
		Put32(record, offset, word);
		offset += sizeof(word);
	}
	for (const auto word: secondary) {
		Put32(record, offset, word);
		offset += sizeof(word);
	}
	const auto hash = XXH3_64bits(record.data(), record.size());
	Put64(record, 32, hash);
	if (!WriteExact(m_private->file, record.data(), record.size()) || !m_private->file.Flush()) {
		error = "cannot append capture event";
		return false;
	}
	m_private->command_dwords += primary.size() + secondary.size();
	return true;
}

bool TraceWriter::Finalize(std::string& error) {
	error.clear();
	if (m_private == nullptr) {
		error = "capture writer is unavailable";
		return false;
	}
	std::lock_guard lock(m_private->mutex);
	if (m_private->finalized) {
		return true;
	}
	if (!m_private->open) {
		error = "capture file is closed";
		return false;
	}
	std::array<uint8_t, FooterSize> footer {};
	std::copy(FooterMagic.begin(), FooterMagic.end(), footer.begin());
	Put64(footer, 8, m_private->sequence);
	Put64(footer, 16, m_private->command_dwords);
	Put64(footer, 24, XXH3_64bits(footer.data(), footer.size()));
	if (!WriteExact(m_private->file, footer.data(), footer.size()) || !m_private->file.Flush()) {
		error = "cannot finalize capture file";
		m_private->file.Close();
		m_private->open = false;
		m_private->reservation.reset();
		return false;
	}
	m_private->file.Close();
	m_private->open = false;
	m_private->reservation.reset();
	if (!SyncParentDirectory(m_path)) {
		error = "cannot durably finalize capture directory";
		return false;
	}
	m_private->finalized = true;
	return true;
}

void TraceWriter::CloseFile() {
	if (m_private == nullptr) {
		return;
	}
	std::lock_guard lock(m_private->mutex);
	if (m_private->open) {
		m_private->file.Close();
		m_private->open = false;
	}
}

bool LoadTrace(const std::filesystem::path& path, Trace& trace, std::string& error,
	           const TraceReadLimits& limits) {
	trace = {};
	error.clear();
	Common::File file;
	if (!file.Open(path, Common::File::Mode::Read)) {
		error = "cannot open capture file";
		return false;
	}
	const auto close = [&file]() { file.Close(); };
	std::array<uint8_t, HeaderSize> header {};
	if (file.Size() < HeaderSize || !ReadExact(file, header.data(), header.size())) {
		close();
		error = "truncated capture header";
		return false;
	}
	if (!std::equal(Magic.begin(), Magic.end(), header.begin()) || Get16(header, 8) != TraceMajorVersion ||
	    Get16(header, 10) > TraceMinorVersion || Get32(header, 12) != HeaderSize ||
	    Get32(header, 16) != static_cast<uint32_t>(TraceCapability::CommandsOnly) ||
	    Get32(header, 20) != 0) {
		close();
		error = "unsupported capture header";
		return false;
	}
	trace.capability = TraceCapability::CommandsOnly;
	uint64_t command_dwords = 0;
	uint64_t last_sequence  = 0;
	while (true) {
		const auto remaining = file.Size() - file.Tell();
		if (remaining == FooterSize) {
			std::array<uint8_t, FooterSize> footer {};
			if (!ReadExact(file, footer.data(), footer.size())) {
				close();
				error = "truncated capture footer";
				return false;
			}
			const auto expected_hash = Get64(footer, 24);
			Put64(footer, 24, 0);
			if (!std::equal(FooterMagic.begin(), FooterMagic.end(), footer.begin()) ||
			    Get64(footer, 8) != trace.events.size() || Get64(footer, 16) != command_dwords ||
			    XXH3_64bits(footer.data(), footer.size()) != expected_hash) {
				close();
				error = "invalid capture footer";
				return false;
			}
			close();
			return true;
		}
		if (remaining == 0) {
			close();
			error = "capture is not finalized";
			return false;
		}
		if (file.Size() - file.Tell() < RecordHeaderSize) {
			close();
			error = "truncated capture event header";
			return false;
		}
		std::array<uint8_t, RecordHeaderSize> record_header {};
		if (!ReadExact(file, record_header.data(), record_header.size())) {
			close();
			error = "cannot read capture event header";
			return false;
		}
		const auto record_size  = Get32(record_header, 0);
		const auto primary_dw   = Get32(record_header, 24);
		const auto secondary_dw = Get32(record_header, 28);
		const auto dwords       = static_cast<uint64_t>(primary_dw) + secondary_dw;
		if (record_size < RecordHeaderSize ||
		    static_cast<uint64_t>(record_size) != RecordHeaderSize + dwords * sizeof(uint32_t) ||
		    record_size - RecordHeaderSize > file.Size() - file.Tell()) {
			close();
			error = "invalid capture event size";
			return false;
		}
		if (trace.events.size() >= limits.max_events ||
		    dwords > limits.max_command_dwords - command_dwords) {
			close();
			error = "capture exceeds configured limits";
			return false;
		}
		std::vector<uint8_t> payload(static_cast<size_t>(dwords * sizeof(uint32_t)));
		if (!payload.empty() && !ReadExact(file, payload.data(), payload.size())) {
			close();
			error = "truncated capture event payload";
			return false;
		}
		uint64_t hash = 0;
		if (!RecordHash(record_header, payload, hash)) {
			close();
			error = "cannot calculate capture event checksum";
			return false;
		}
		if (hash != Get64(record_header, 32)) {
			close();
			error = "capture event checksum mismatch";
			return false;
		}
		TraceEvent event;
		event.type                      = static_cast<TraceEventType>(Get32(record_header, 4));
		event.sequence                  = Get64(record_header, 8);
		event.queue                     = Get32(record_header, 16);
		const auto flags                = Get32(record_header, 20);
		event.trigger_interrupt_on_done = (flags & InterruptFlag) != 0;
		if (!IsKnownType(event.type) || (flags & ~InterruptFlag) != 0 ||
		    event.sequence != last_sequence + 1) {
			close();
			error = "invalid capture event metadata";
			return false;
		}
		event.primary.resize(primary_dw);
		event.secondary.resize(secondary_dw);
		size_t offset = 0;
		for (auto& word: event.primary) {
			word = Get32(payload, offset);
			offset += sizeof(word);
		}
		for (auto& word: event.secondary) {
			word = Get32(payload, offset);
			offset += sizeof(word);
		}
		if (!ValidateEventShape(event.type, event.queue, event.trigger_interrupt_on_done,
		                        event.primary.size(), event.secondary.size(), error)) {
			close();
			return false;
		}
		last_sequence = event.sequence;
		command_dwords += dwords;
		trace.events.push_back(std::move(event));
	}
}

} // namespace Libs::Graphics::Capture
