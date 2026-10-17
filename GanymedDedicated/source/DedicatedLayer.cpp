#include "DedicatedLayer.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Net/Net.h"
#include "GanymedE/Net/NetTest.h"
#include "GanymedE/Net/World.h"
#include "GanymedE/Scene/SceneSerializer.h"

#include <cstring>
#include <filesystem>
#include <thread>

namespace GanymedE {

	DedicatedLayer::DedicatedLayer(DedicatedConfig config)
		: Layer("DedicatedLayer"), m_Config(std::move(config))
	{
	}

	void DedicatedLayer::OnAttach()
	{
		GE_PROFILE_FUNCTION();

		ServerLaunch launch;
		launch.ServerId = m_Config.ServerId;
		launch.Agent = m_Config.Agent;
		while (!launch.Agent.empty() && launch.Agent.back() == '/')
			launch.Agent.pop_back();
		launch.Port = m_Config.GamePort;
		launch.Advertise = m_Config.Advertise;
		launch.Outcome = m_Config.Outcome;
		launch.Dev = m_Config.Dev;
		std::memcpy(launch.PublicKey, m_Config.PublicKey, sizeof(launch.PublicKey));

		Net::SetSim(m_Config.Sim);

		std::string error;
		if (!m_Server.Start(launch, error))
		{
			GE_CORE_ERROR("Dedicated server failed to start: {0}", error);
			Application::Get().Close();
			return;
		}

		if (!m_Config.Dev && !m_Server.WaitForAllocation(error))
		{
			GE_CORE_ERROR("No allocation: {0}", error);
			Application::Get().Close();
			return;
		}

		// Admission is open before the load. A HELLO during those seconds is answered, and health
		// is on its own thread, which is what the slow-load check exists to prove.
		if (m_Config.LoadDelaySeconds > 0.0f)
		{
			GE_CORE_INFO("Artificial scene-load delay of {0} s", m_Config.LoadDelaySeconds);
			std::this_thread::sleep_for(std::chrono::duration<double>(m_Config.LoadDelaySeconds));
		}

		if (m_Server.IsStopped())
		{
			GE_CORE_ERROR("Stopped during scene load");
			m_Server.GiveUp();
			Application::Get().Close();
			return;
		}

		AssetManager::Init(/*writableAssets=*/false, m_Config.AssetRoot);

		std::filesystem::path scenePath = m_Config.Scene;
		if (!scenePath.is_absolute())
			scenePath = GetAssetRoot() / scenePath;

		m_Scene = CreateRef<Scene>(SceneRole::Server);
		SceneSerializer serializer(m_Scene);
		if (!serializer.Deserialize(scenePath.generic_string()))
		{
			GE_CORE_ERROR("Failed to load scene '{0}'", scenePath.generic_string());
			m_Server.GiveUp();
			Application::Get().Close();
			return;
		}

		const size_t entityCount = m_Scene->Reg().view<TagComponent>().size();
		GE_CORE_INFO("Scene '{0}' loaded ({1} entities)", scenePath.generic_string(), entityCount);

		m_Scene->OnViewportResize(1280, 720);
		m_Scene->OnRuntimeStart();
		m_RuntimeStarted = true;
		if (!m_Config.NetTest.empty())
			Net::Test::Start(*m_Scene, m_Config.NetTest);
		m_Simulating = true;
		if (m_Config.Dev)
		{
			GE_CORE_INFO("Dev server up on {0}; it runs until killed", m_Config.Advertise);
		}
		else
		{
			m_MatchEnd = std::chrono::steady_clock::now()
				+ std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<double>(m_Config.MatchSeconds));
			if (m_Config.MatchSeconds > 0.0f)
				GE_CORE_INFO("Match running for {0} s, outcome {1} unless the game ends it first",
					m_Config.MatchSeconds, m_Config.Outcome);
			else
				GE_CORE_INFO("Match running until the game ends it (Net.EndMatch)");
		}
	}

	void DedicatedLayer::OnDetach()
	{
		if (m_RuntimeStarted && m_Scene)
			m_Scene->OnRuntimeStop();
		m_Scene.reset();

		// A refused agent call is not a finished match. Finish would post the configured
		// outcome and then retry it. GiveUp posts shutdown once, or nothing if teardown
		// already happened. A layer destroyed mid-match while the agent is still answering
		// does report the outcome: the process is going away with the match still open.
		if (m_Simulating && !m_Server.IsStopped())
			m_Server.Finish();
		else
			m_Server.GiveUp();

		m_Simulating = false;
		AssetManager::Shutdown();
	}

	void DedicatedLayer::OnUpdate(Timestep ts)
	{
		if (!m_Simulating || !m_Scene)
			return;

		if (!m_Config.NetTest.empty())
			Net::Test::Advance(*m_Scene);
		m_Scene->OnUpdateRuntime(ts);

		// The game ended the match (Net.EndMatch). Its outcome replaces the configured one.
		std::string decided;
		const bool ended = Net::TakeMatchEnd(decided);

		// Dev has no agent. Killing the process is the stop; a decided match is only logged.
		if (m_Config.Dev)
		{
			if (ended && !m_DevEndLogged)
			{
				m_DevEndLogged = true;
				GE_CORE_INFO("Match ended by the game: {0} (dev: nothing is reported)", decided);
			}
			return;
		}

		const bool refused = m_Server.IsStopped();
		// MatchSeconds 0 is "no clock": the game ends the match.
		const bool elapsed = m_Config.MatchSeconds > 0.0f && std::chrono::steady_clock::now() >= m_MatchEnd;
		if (!refused && !elapsed && !ended)
			return;
		if (ended)
		{
			GE_CORE_INFO("Match ended by the game: {0}", decided);
			m_Server.SetOutcome(decided);
		}

		m_Simulating = false;
		if (refused)
		{
			GE_CORE_ERROR("Agent refused a call; leaving without a result");
			m_Server.GiveUp();
		}
		else
			m_Server.Finish();
		Application::Get().Close();
	}

}
