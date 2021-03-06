/*
 * Copyright (c) 2020 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shelly_switch.hpp"

#include "mgos.h"

#include "shelly_hap_accessory.hpp"
#include "shelly_hap_chars.hpp"

#define SHELLY_HAP_IID_BASE_SWITCH 0x100
#define SHELLY_HAP_IID_STEP_SWITCH 4

namespace shelly {

ShellySwitch::ShellySwitch(int id, Input *in, Output *out, PowerMeter *out_pm,
                           struct mgos_config_sw *cfg)
    : Component(id),
      in_(in),
      out_(out),
      out_pm_(out_pm),
      cfg_(cfg),
      auto_off_timer_id_(MGOS_INVALID_TIMER_ID) {
}

ShellySwitch::~ShellySwitch() {
  if (auto_off_timer_id_ != MGOS_INVALID_TIMER_ID) {
    mgos_clear_timer(auto_off_timer_id_);
    auto_off_timer_id_ = MGOS_INVALID_TIMER_ID;
  }
  if (in_ != nullptr) {
    in_->RemoveHandler(handler_id_);
  }
}

Component::Type ShellySwitch::type() const {
  return Type::kSwitch;
}

StatusOr<std::string> ShellySwitch::GetInfo() const {
  std::string res = mgos::JSONPrintStringf(
      "{id: %d, type: %d, name: %Q, svc_type: %d, in_mode: %d, initial: %d, "
      "state: %B, auto_off: %B, auto_off_delay: %.3f",
      id(), type(), (cfg_->name ? cfg_->name : ""), cfg_->svc_type,
      cfg_->in_mode, cfg_->initial_state, out_->GetState(), cfg_->auto_off,
      cfg_->auto_off_delay);
  if (out_pm_ != nullptr) {
    auto power = out_pm_->GetPowerW();
    if (power.ok()) {
      mgos::JSONAppendStringf(&res, ", apower: %.3f", power.ValueOrDie());
    }
    auto energy = out_pm_->GetEnergyWH();
    if (energy.ok()) {
      mgos::JSONAppendStringf(&res, ", aenergy: %.3f", energy.ValueOrDie());
    }
  }
  res.append("}");
  return res;
}

Status ShellySwitch::SetConfig(const std::string &config_json,
                               bool *restart_required) {
  struct mgos_config_sw cfg = *cfg_;
  cfg.name = nullptr;
  json_scanf(config_json.c_str(), config_json.size(),
             "{name: %Q, svc_type: %d, in_mode: %d, initial_state: %d, "
             "auto_off: %B, auto_off_delay: %lf}",
             &cfg.name, &cfg.svc_type, &cfg.in_mode, &cfg.initial_state,
             &cfg.auto_off, &cfg.auto_off_delay);
  mgos::ScopedCPtr name_owner((void *) cfg.name);
  // Validation.
  if (cfg.name != nullptr && strlen(cfg.name) > 64) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s",
                        "name (too long, max 64)");
  }
  if (cfg.svc_type < -1 || cfg.svc_type > 2) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "svc_type");
  }
  if (cfg.in_mode < 0 || cfg.in_mode > 3) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "in_mode");
  }
  if (cfg.initial_state < 0 || cfg.initial_state > 3) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  cfg.auto_off = (cfg.auto_off != 0);
  if (cfg.initial_state < 0 || cfg.initial_state > 3) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  // Now copy over.
  *restart_required = false;
  if (cfg_->name != nullptr && strcmp(cfg_->name, cfg.name) != 0) {
    mgos_conf_set_str(&cfg_->name, cfg.name);
    *restart_required = true;
  }
  if (cfg_->svc_type != cfg.svc_type) {
    cfg_->svc_type = cfg.svc_type;
    *restart_required = true;
  }
  if (cfg_->in_mode != cfg.in_mode) {
    if (cfg_->in_mode == (int) InMode::kDetached ||
        cfg.in_mode == (int) InMode::kDetached) {
      *restart_required = true;
    }
    cfg_->in_mode = cfg.in_mode;
  }
  cfg_->initial_state = cfg.initial_state;
  cfg_->auto_off = cfg.auto_off;
  cfg_->auto_off_delay = cfg.auto_off_delay;
  return Status::OK();
}

Status ShellySwitch::Init() {
  if (!cfg_->enable) {
    LOG(LL_INFO, ("'%s' is disabled", cfg_->name));
    return Status::OK();
  }
  switch (static_cast<InitialState>(cfg_->initial_state)) {
    case InitialState::kOff:
      SetState(false, "init");
      break;
    case InitialState::kOn:
      SetState(true, "init");
      break;
    case InitialState::kLast:
      SetState(cfg_->state, "init");
      break;
    case InitialState::kInput:
      if (in_ != nullptr &&
          cfg_->in_mode == static_cast<int>(InMode::kToggle)) {
        SetState(in_->GetState(), "init");
      }
      break;
  }
  LOG(LL_INFO, ("Exporting '%s': type %d, state: %d", cfg_->name,
                cfg_->svc_type, out_->GetState()));
  if (in_ != nullptr) {
    handler_id_ = in_->AddHandler(
        std::bind(&ShellySwitch::InputEventHandler, this, _1, _2));
  }
  return Status::OK();
}

void ShellySwitch::SetState(bool new_state, const char *source) {
  SetStateInternal(new_state, source, false /* is_auto_off */);
}

void ShellySwitch::SetStateInternal(bool new_state, const char *source,
                                    bool is_auto_off) {
  bool cur_state = out_->GetState();
  out_->SetState(new_state, source);
  if (cfg_->state != new_state) {
    cfg_->state = new_state;
    mgos_sys_config_save(&mgos_sys_config, false /* try_once */,
                         NULL /* msg */);
  }
  if (new_state == cur_state) return;
  for (auto *c : state_notify_chars_) {
    c->RaiseEvent();
  }

  if (auto_off_timer_id_ != MGOS_INVALID_TIMER_ID) {
    // Cancel timer if state changes so that only the last timer is triggered if
    // state changes multiple times
    mgos_clear_timer(auto_off_timer_id_);
    auto_off_timer_id_ = MGOS_INVALID_TIMER_ID;
  }

  if (cfg_->auto_off && !is_auto_off) {
    auto_off_timer_id_ =
        mgos_set_timer(cfg_->auto_off_delay * 1000, 0, AutoOffTimerCB, this);
    LOG(LL_INFO,
        ("%d: Set auto-off timer for %.3f", id(), cfg_->auto_off_delay));
  }
}

// static
void ShellySwitch::AutoOffTimerCB(void *ctx) {
  ShellySwitch *sw = static_cast<ShellySwitch *>(ctx);
  sw->auto_off_timer_id_ = MGOS_INVALID_TIMER_ID;
  if (sw->cfg_->auto_off) {
    // Don't set state if auto off has been disabled during timer run
    sw->SetStateInternal(false, "auto_off", true /* is_auto_off */);
  }
}

void ShellySwitch::InputEventHandler(Input::Event ev, bool state) {
  if (ev != Input::Event::kChange) return;
  switch (static_cast<InMode>(cfg_->in_mode)) {
    case InMode::kMomentary:
      if (state) {  // Only on 0 -> 1 transitions.
        SetState(!out_->GetState(), "button");
      }
      break;
    case InMode::kToggle:
      SetState(state, "switch");
      break;
    case InMode::kEdge:
      SetState(!out_->GetState(), "button");
      break;
    case InMode::kDetached:
      // Nothing to do
      break;
  }
}

}  // namespace shelly
