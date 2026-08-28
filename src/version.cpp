#include "sys.h"
#include <openkal/version.h>

// What this implementation says about itself before it is used. Both answers are
// constants; openkal/version.h states why they belong to no interface.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

kal_u64 kal_interfaces(void) {
    // Written out rather than derived: a word derived from what happens to be
    // linked would report a facility as present when the linker had merely
    // kept it.
    return KAL_IFACE_ABORT  | KAL_IFACE_STREAM   | KAL_IFACE_MEMORY
         | KAL_IFACE_ENV    | KAL_IFACE_TIME     | KAL_IFACE_RANDOM
         | KAL_IFACE_FS     | KAL_IFACE_PROCESS  | KAL_IFACE_TASK
         | KAL_IFACE_EXEC   | KAL_IFACE_TERMINAL | KAL_IFACE_NET
         | KAL_IFACE_DATAGRAM | KAL_IFACE_SPACE  | KAL_IFACE_TIMEOUT;
}

}
