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

#include <algorithm>

#include "shelly_main.hpp"

namespace shelly {

extern void PowerMeterInit(std::vector<std::unique_ptr<PowerMeter>> *pms);

void CreatePeripherals(std::vector<std::unique_ptr<Input>> *inputs,
                       std::vector<std::unique_ptr<Output>> *outputs,
                       std::vector<std::unique_ptr<PowerMeter>> *pms) {
  // Note: SW2 output (GPIO15) must be initialized before
  // SW1 input (GPIO13), doing it in reverse turns on SW2.
  outputs->emplace_back(new OutputPin(1, 4, 1));
  outputs->emplace_back(new OutputPin(2, 15, 1));
  auto *in1 = new InputPin(1, 13, 1, MGOS_GPIO_PULL_NONE, true);
  in1->AddHandler(std::bind(&HandleInputResetSequence, in1, 4, _1, _2));
  inputs->emplace_back(in1);
  auto *in2 = new InputPin(2, 5, 1, MGOS_GPIO_PULL_NONE, true);
  in2->AddHandler(std::bind(&HandleInputResetSequence, in2, 15, _1, _2));
  inputs->emplace_back(in2);
  PowerMeterInit(pms);
}

void CreateComponents(std::vector<Component *> *comps,
                      std::vector<std::unique_ptr<hap::Accessory>> *accs,
                      HAPAccessoryServerRef *svr) {
  // Use legacy layout if upgraded from an older version (pre-2.1).
  // However, presence of detached inputs overrides it.
  bool compat_20 = (mgos_sys_config_get_shelly_legacy_hap_layout() &&
                    mgos_sys_config_get_sw1_in_mode() != 3 &&
                    mgos_sys_config_get_sw2_in_mode() != 3);
  if (!compat_20) {
    CreateHAPSwitch(1, mgos_sys_config_get_sw1(), mgos_sys_config_get_ssw1(),
                    comps, accs, svr, false /* to_pri_acc */);
    CreateHAPSwitch(2, mgos_sys_config_get_sw2(), mgos_sys_config_get_ssw2(),
                    comps, accs, svr, false /* to_pri_acc */);
  } else {
    CreateHAPSwitch(2, mgos_sys_config_get_sw2(), mgos_sys_config_get_ssw2(),
                    comps, accs, svr, true /* to_pri_acc */);
    CreateHAPSwitch(1, mgos_sys_config_get_sw1(), mgos_sys_config_get_ssw1(),
                    comps, accs, svr, true /* to_pri_acc */);
    std::reverse(comps->begin(), comps->end());
  }
}

}  // namespace shelly
