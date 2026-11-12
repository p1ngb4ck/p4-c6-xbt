#pragma once

#include "esphome/components/bt_offload/bt_offload.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/components/text/text.h"
#include "esphome/core/component.h"

namespace esphome::bt_offload {

class BTOffloadText : public text::Text, public Component {
 public:
  void set_parent(BTOffload *parent) { this->parent_ = parent; }
  void set_code_entry(bool code_entry) { this->code_entry_ = code_entry; }
  void setup() override { this->publish_state(this->code_entry_ ? "" : this->parent_->preset_pin()); }
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void control(const std::string &value) override {
    if (this->code_entry_) {
      this->parent_->pair_reply(value);
      this->publish_state("");
      return;
    }
    this->parent_->set_preset_pin(value);
    this->publish_state(this->parent_->preset_pin());
  }

  BTOffload *parent_{nullptr};
  bool code_entry_{false};
};

}

#endif
