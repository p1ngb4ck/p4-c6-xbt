#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/components/number/number.h"
#include "esphome/core/helpers.h"

namespace esphome::bt_offload {

class BTOffload;

class BTOffloadDelayNumber : public number::Number, public Parented<BTOffload> {
 protected:
  void control(float value) override;
};

}

#endif
