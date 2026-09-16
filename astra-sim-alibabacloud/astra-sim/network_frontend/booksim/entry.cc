/*
 *Copyright (c) 2024, Alibaba Group;
 *Licensed under the Apache License, Version 2.0 (the "License");
 *you may not use this file except in compliance with the License.
 *You may obtain a copy of the License at

 *   http://www.apache.org/licenses/LICENSE-2.0

 *Unless required by applicable law or agreed to in writing, software
 *distributed under the License is distributed on an "AS IS" BASIS,
 *WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *See the License for the specific language governing permissions and
 *limitations under the License.
*/

#include "entry.h"

#include <cassert>

std::map<std::pair<int, std::pair<int, int> >, struct task1> expeRecvHash;
std::map<std::pair<int, std::pair<int, int> >, uint64_t>     recvHash;
std::map<std::pair<int, std::pair<int, int> >, struct task1> sentHash;
std::map<std::pair<int, int>, int64_t>                       nodeHash;
std::map<std::pair<std::pair<int, int>, int>, AstraSim::ncclFlowTag>
                                                             receiver_pending_queue;

void notify_receiver_receive_data(int sender_node, int receiver_node,
                                  uint64_t message_size,
                                  AstraSim::ncclFlowTag flowTag) {
  MockNcclLog * NcclLog = MockNcclLog::getInstance();
  const int tag = flowTag.tag_id;
  const std::pair<int, std::pair<int, int> > key =
      std::make_pair(tag, std::make_pair(sender_node, receiver_node));

  std::map<std::pair<int, std::pair<int, int> >, struct task1>::iterator it =
      expeRecvHash.find(key);

  if(it != expeRecvHash.end()) {
    task1 t2 = it->second;
    AstraSim::RecvPacketEventHadndlerData * ehd =
        (AstraSim::RecvPacketEventHadndlerData *)t2.fun_arg;

    NcclLog->writeLog(NcclLogLevel::DEBUG,
        " %d notify receiver: %d message size: %lu t2.count: %lu channel id: %d",
        sender_node, receiver_node, message_size, t2.count, flowTag.channel_id);

    if(message_size == t2.count) {
      expeRecvHash.erase(it);
      // The receive was posted with current_flow_id/child_flow_id == -1; the
      // real identity comes from the SENDER's tag. NcclFlowModel::run reads
      // current_flow_id, channel_id and tree_flow_list off this and uses
      // tree_flow_list to decrement indegree_mapping and release the successor
      // flows. Leave the -1s in place and the collective's DAG stalls partway
      // through, with the simulator reporting a clean finish.
      assert(ehd->flowTag.current_flow_id == -1 && ehd->flowTag.child_flow_id == -1);
      ehd->flowTag = flowTag;
      t2.msg_handler(t2.fun_arg);
    } else if(message_size > t2.count) {
      recvHash[key] = message_size - t2.count;
      expeRecvHash.erase(it);
      assert(ehd->flowTag.current_flow_id == -1 && ehd->flowTag.child_flow_id == -1);
      ehd->flowTag = flowTag;
      t2.msg_handler(t2.fun_arg);
    } else {
      // Partial delivery: keep the posted receive, waiting for the remainder.
      t2.count -= message_size;
      expeRecvHash[key] = t2;
    }
  } else {
    // Data beat the matching sim_recv. Park both the bytes and the sender's tag
    // so sim_recv can complete immediately when it is finally posted.
    receiver_pending_queue[std::make_pair(
        std::make_pair(receiver_node, sender_node), tag)] = flowTag;
    if(recvHash.find(key) == recvHash.end()) {
      recvHash[key] = message_size;
    } else {
      recvHash[key] += message_size;
    }
  }

  std::pair<int, int> nk = std::make_pair(receiver_node, 1);
  if(nodeHash.find(nk) == nodeHash.end()) {
    nodeHash[nk] = message_size;
  } else {
    nodeHash[nk] += message_size;
  }
}

void notify_sender_sending_finished(int sender_node, int receiver_node,
                                    uint64_t message_size,
                                    AstraSim::ncclFlowTag flowTag) {
  MockNcclLog * NcclLog = MockNcclLog::getInstance();
  const int tag = flowTag.tag_id;
  const std::pair<int, std::pair<int, int> > key =
      std::make_pair(tag, std::make_pair(sender_node, receiver_node));

  std::map<std::pair<int, std::pair<int, int> >, struct task1>::iterator it =
      sentHash.find(key);
  if(it == sentHash.end()) {
    NcclLog->writeLog(NcclLogLevel::ERROR,
        "sentHash can't find sender_node %d receiver_node %d message_size %lu",
        sender_node, receiver_node, message_size);
    return;
  }

  task1 t2 = it->second;
  AstraSim::SendPacketEventHandlerData * ehd =
      (AstraSim::SendPacketEventHandlerData *)t2.fun_arg;
  ehd->flowTag = flowTag;

  if(t2.count != message_size) {
    NcclLog->writeLog(NcclLogLevel::ERROR,
        "sentHash msg size mismatch sender_node %d receiver_node %d message_size %lu",
        sender_node, receiver_node, message_size);
    return;
  }

  sentHash.erase(it);
  std::pair<int, int> nk = std::make_pair(sender_node, 0);
  if(nodeHash.find(nk) == nodeHash.end()) {
    nodeHash[nk] = message_size;
  } else {
    nodeHash[nk] += message_size;
  }
  t2.msg_handler(t2.fun_arg);
}
