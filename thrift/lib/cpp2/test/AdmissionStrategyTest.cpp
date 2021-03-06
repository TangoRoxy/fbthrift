/*
 * Copyright 2018-present Facebook, Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <thrift/lib/cpp2/server/admission_strategy/AdmissionStrategy.h>

#include <thrift/lib/cpp2/server/Cpp2ConnContext.h>
#include <thrift/lib/cpp2/server/admission_strategy/GlobalAdmissionStrategy.h>
#include <thrift/lib/cpp2/server/admission_strategy/PerClientIdAdmissionStrategy.h>
#include <thrift/lib/cpp2/server/admission_strategy/PriorityAdmissionStrategy.h>
#include <thrift/lib/cpp2/server/admission_strategy/WhitelistAdmissionStrategy.h>

#include <chrono>

#include <gtest/gtest.h>

#include <folly/Random.h>

#include <thrift/lib/cpp2/test/util/FakeClock.h>

using namespace apache::thrift;

using apache::thrift::transport::THeader;

namespace apache {
namespace thrift {

FakeClock::time_point FakeClock::now_us_;

class DummyController : public AdmissionController {
 public:
  bool admit() override {
    return true;
  }
  void dequeue() override {}
  void returnedResponse() override {}
};

class AdmissionControllerSelectorTest : public testing::Test {
 public:
  const std::string kClientId{"client_id"};
};

TEST_F(AdmissionControllerSelectorTest, globalAdmission) {
  GlobalAdmissionStrategy selector(std::make_shared<DummyController>());

  THeader headerA1;
  headerA1.setReadHeaders({{kClientId, "A"}});
  auto admissionControllerA1 = selector.select("myThriftMethod", &headerA1);

  THeader headerA2;
  headerA2.setReadHeaders({{kClientId, "A"}});
  auto admissionControllerA2 = selector.select("myThriftMethod", &headerA2);

  ASSERT_EQ(admissionControllerA1, admissionControllerA2);

  THeader headerB1;
  headerB1.setReadHeaders({{kClientId, "B"}});
  auto admissionControllerB1 = selector.select("myThriftMethod", &headerB1);

  ASSERT_EQ(admissionControllerA1, admissionControllerB1);

  THeader headerNoClientId;
  auto admissionControllerNoClientId =
      selector.select("myThriftMethod", &headerNoClientId);

  ASSERT_EQ(admissionControllerB1, admissionControllerNoClientId);
}

TEST_F(AdmissionControllerSelectorTest, perClientIdAdmission) {
  PerClientIdAdmissionStrategy selector(
      [](auto&) { return std::make_shared<DummyController>(); }, kClientId);

  THeader headerA1;
  headerA1.setReadHeaders({{kClientId, "A"}});
  auto admissionControllerA1 = selector.select("myThriftMethod", &headerA1);

  THeader headerA2;
  headerA2.setReadHeaders({{kClientId, "A"}});
  auto admissionControllerA2 = selector.select("myThriftMethod", &headerA2);

  ASSERT_EQ(admissionControllerA1, admissionControllerA2);

  THeader headerB1;
  headerB1.setReadHeaders({{kClientId, "B"}});
  auto admissionControllerB1 = selector.select("myThriftMethod", &headerB1);

  ASSERT_NE(admissionControllerA1, admissionControllerB1);

  THeader headerB2;
  headerB2.setReadHeaders({{kClientId, "B"}});
  auto admissionControllerB2 = selector.select("myThriftMethod", &headerB2);

  ASSERT_EQ(admissionControllerB1, admissionControllerB2);
}

TEST_F(AdmissionControllerSelectorTest, priorityBasedAdmission) {
  std::unordered_map<std::string, uint8_t> priorities = {
      {"A", 1}, {"B", 5}, {"*", 1}};
  PriorityAdmissionStrategy selector(
      priorities,
      []() { return std::make_shared<DummyController>(); },
      kClientId);

  std::map<std::string, std::set<std::shared_ptr<AdmissionController>>>
      mapping = {{"A", std::set<std::shared_ptr<AdmissionController>>()},
                 {"B", std::set<std::shared_ptr<AdmissionController>>()},
                 {"*", std::set<std::shared_ptr<AdmissionController>>()}};

  for (auto& it : mapping) {
    auto& clientId = it.first;
    auto& admControllerSet = it.second;
    for (int i = 0; i < 5; i++) {
      THeader header;
      header.setReadHeaders({{kClientId, clientId}});
      auto controller = selector.select("myThriftMethod", &header);
      admControllerSet.insert(controller);
    }
  }

  // the # of admission controllers should be equal to the priority assigned
  // to a specific client_id
  for (auto& it : priorities) {
    ASSERT_EQ(mapping[it.first].size(), it.second);
  }
}

TEST_F(AdmissionControllerSelectorTest, deniesZeroPriority) {
  std::unordered_map<std::string, uint8_t> priorities = {{"A", 2}, {"B", 0}};
  PriorityAdmissionStrategy selector(
      priorities,
      []() { return std::make_shared<DummyController>(); },
      kClientId);

  std::map<std::string, std::set<std::shared_ptr<AdmissionController>>>
      mapping = {{"A", std::set<std::shared_ptr<AdmissionController>>()},
                 {"B", std::set<std::shared_ptr<AdmissionController>>()}};

  for (auto& it : mapping) {
    auto& clientId = it.first;
    auto& admControllerSet = it.second;
    for (int i = 0; i < 5; i++) {
      THeader header;
      header.setReadHeaders({{kClientId, clientId}});
      auto controller = selector.select("myThriftMethod", &header);
      admControllerSet.insert(controller);
    }
  }

  THeader header;
  auto controllerForEmpty = selector.select("myThriftMethod", &header);

  THeader headerC;
  headerC.setReadHeaders({{kClientId, "C"}});
  auto controllerForC = selector.select("myThriftMethod", &headerC);

  ASSERT_FALSE(controllerForC->admit());
  ASSERT_EQ(controllerForEmpty, controllerForC);

  // the # of admission controllers should be equal to the priority assigned
  // to a specific client_id (or 1 for the deny controller)
  ASSERT_EQ(mapping["A"].size(), 2);
  ASSERT_EQ(mapping["B"].size(), 1);
  auto admControllerForB = *mapping["B"].begin();
  ASSERT_FALSE(admControllerForB->admit());
}

TEST_F(AdmissionControllerSelectorTest, whiteListAdmission) {
  std::unordered_set<std::string> whitelist{"getStatus"};
  WhitelistAdmissionStrategy<GlobalAdmissionStrategy> selector(
      whitelist, std::make_shared<DummyController>());

  THeader header;
  auto admissionController = selector.select("myThriftMethod", &header);
  ASSERT_NE(dynamic_cast<DummyController*>(admissionController.get()), nullptr);

  auto admissionController2 = selector.select("getStatus", &header);
  ASSERT_NE(
      dynamic_cast<AcceptAllAdmissionController*>(admissionController2.get()),
      nullptr);
}

} // namespace thrift
} // namespace apache
