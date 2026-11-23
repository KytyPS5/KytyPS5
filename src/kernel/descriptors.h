#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_DESCRIPTORS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_DESCRIPTORS_H_

namespace Libs::LibKernel::Descriptors {

// Descriptors 0-2 are the standard streams.
constexpr int DESCRIPTOR_MIN = 3;

// Files, sockets, epolls and event queues share one process descriptor table, and a new
// descriptor takes the lowest free number.
int  Allocate();
void Release(int d);

} // namespace Libs::LibKernel::Descriptors

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_DESCRIPTORS_H_ */
