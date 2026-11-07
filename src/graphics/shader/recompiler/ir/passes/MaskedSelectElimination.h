#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// A guest VALU write under EXEC becomes SELECT(EXEC, new, old): the lanes outside EXEC keep the
// register's old value. Most such writes are temporaries that only feed further writes under the
// same EXEC, so the lanes outside EXEC never read them, and the select only costs host ALU (a
// third of the vector instructions of some FC 25 compute shaders).
//
// SELECT(c, a, b) becomes `a` when its value is not needed in the lanes where `c` is false: every
// use is the selected operand of a SELECT whose condition implies `c`, the operand of a lane-local
// operation whose own value is not needed there, or the address or data of a memory access whose
// predicate implies `c` (the access does not happen in a lane where its predicate is false). A
// lane operation, a PHI, an image operation or any other use keeps the select.
//
// A register written twice under the same EXEC chains its selects: the second one's old value is
// the first select, which is the first one's old value again where EXEC is false. The second select
// takes that old value directly, so the first one is often left without uses.
// Returns the number of selects removed or skipped.
[[nodiscard]] uint32_t EliminateMaskedSelects(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
