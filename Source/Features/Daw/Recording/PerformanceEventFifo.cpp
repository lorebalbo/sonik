//==============================================================================
// PRD-0072: Performance-Event Bridge.
//
// The transport is fully defined inline in the header (a fixed-size POD FIFO
// with no out-of-line state). This translation unit exists so the type has a
// compilation anchor in the build and validates the header compiles standalone.
//==============================================================================

#include "PerformanceEventFifo.h"

namespace Daw
{

// Compile-time guarantees re-asserted in the TU for documentation/CI value.
static_assert (std::is_trivially_copyable_v<PerformanceEvent>,
               "PerformanceEvent must remain trivially copyable for lock-free FIFO use");
static_assert (PerformanceEventFifo::StorageSize == PerformanceEventFifo::Capacity + 1,
               "AbstractFifo needs one spare slot to distinguish full from empty");

} // namespace Daw
