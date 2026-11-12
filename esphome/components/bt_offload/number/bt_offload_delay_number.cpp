#include "bt_offload_delay_number.h"

#ifdef USE_BT_OFFLOAD

#include "../bt_offload.h"

namespace esphome::bt_offload {

void BTOffloadDelayNumber::control(float value) {
  this->parent_->set_output_delay_ms(static_cast<int32_t>(value < 0 ? value - 0.5f : value + 0.5f));
}

}

#endif
