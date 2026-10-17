#pragma once

/// @file Collab/Collab.hpp
/// @brief 协作层伞形头（Docs/01 §3.2、§5）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include "Collab/AgentSpec.hpp"
#include "Collab/CollaborationEvent.hpp"
#include "Collab/MessageBus.hpp"
#include "Collab/ICollaboration.hpp"
#include "Collab/Critic.hpp"
#include "Collab/TaskAssigner.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/OrchestratorWorker.hpp"
#include "Collab/Supervisor.hpp"
#include "Collab/GroupChat.hpp"
#include "Collab/Debate.hpp"
#include "Collab/PeerToPeer.hpp"
#include "Collab/SequentialWorkflow.hpp"
#include "Collab/CollabSelector.hpp"
