#include "gepch.h"
#include "EditorCamera.h"
#include "GanymedE/Renderer/Renderer.h"

#include "GanymedE/Core/Input.h"
#include "GanymedE/Core/KeyCodes.h"
#include "GanymedE/Core/MouseButtonCodes.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace GanymedE {

	namespace {
		constexpr float kMinOrthoHeight = 1.0f;
		constexpr float kMaxOrthoHeight = 500.0f;
		constexpr float kTopPitch = -1.57079637f; // -π/2; pitch lock for Top (Ortho)
		constexpr float kMinOrthoDistance = 50.0f;
	}

	EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
		: m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip),
		Camera(Projection::Perspective(glm::radians(fov), aspectRatio, nearClip, farClip))
	{
		UpdateView();
	}

	void EditorCamera::UpdateProjection()
	{
		m_AspectRatio = m_ViewportHeight > 0.0f ? (m_ViewportWidth / m_ViewportHeight) : 1.0f;
		if (m_Orthographic)
		{
			const float halfH = m_OrthoHeight * 0.5f;
			const float halfW = halfH * m_AspectRatio;
			m_Projection = Projection::Orthographic(-halfW, halfW, -halfH, halfH,
				m_NearClip, m_FarClip);
		}
		else
		{
			m_Projection = Projection::Perspective(glm::radians(m_FOV), m_AspectRatio,
				m_NearClip, m_FarClip);
		}
	}

	void EditorCamera::SetOrthographic(bool enabled)
	{
		if (enabled == m_Orthographic)
		{
			if (enabled)
				m_Pitch = kTopPitch;
			UpdateProjection();
			UpdateView();
			return;
		}

		if (enabled)
		{
			m_SavedPitch = m_Pitch;
			m_SavedDistance = m_Distance;
			const float halfFov = glm::radians(m_FOV) * 0.5f;
			const float t = glm::tan(halfFov);
			m_OrthoHeight = (t > 1.0e-4f) ? (2.0f * m_Distance * t) : 40.0f;
			m_OrthoHeight = glm::clamp(m_OrthoHeight, kMinOrthoHeight, kMaxOrthoHeight);
			m_Distance = glm::max(m_Distance, kMinOrthoDistance);
			m_Pitch = kTopPitch;
			m_Orthographic = true;
		}
		else
		{
			m_Orthographic = false;
			m_Pitch = m_SavedPitch;
			m_Distance = m_SavedDistance;
		}

		UpdateProjection();
		UpdateView();
	}

	void EditorCamera::UpdateView()
	{
		m_Position = CalculatePosition();

		glm::quat orientation = GetOrientation();
		m_ViewMatrix = glm::translate(glm::mat4(1.0f), m_Position) * glm::mat4_cast(orientation);
		m_ViewMatrix = glm::inverse(m_ViewMatrix);
	}

	std::pair<float, float> EditorCamera::PanSpeed() const
	{
		float x = std::min(m_ViewportWidth / 1000.0f, 2.4f); // max = 2.4f
		float xFactor = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		float y = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		float yFactor = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		return { xFactor, yFactor };
	}

	float EditorCamera::RotationSpeed() const
	{
		return 0.8f;
	}

	float EditorCamera::ZoomSpeed() const
	{
		if (m_Orthographic)
			return glm::clamp(m_OrthoHeight * 0.15f, 0.2f, 40.0f);

		float distance = m_Distance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}

	void EditorCamera::OnUpdate(Timestep ts)
	{
		const glm::vec2& mouse = Input::GetMousePosition();
		glm::vec2 delta = (mouse - m_InitialMousePosition) * 0.003f;
		m_InitialMousePosition = mouse;

		// RMB fly mode (no Alt): look + WASD/QE move. Disabled in top-down: pitch is
		// locked and a fly camera fighting the plan view is the wrong tool.
		if (!m_Orthographic && Input::IsMouseButtonPressed(Mouse::ButtonRight)
			&& !Input::IsKeyPressed(Key::LeftAlt))
		{
			MouseRotate(delta);
			m_Pitch = std::clamp(m_Pitch, -1.5533f, 1.5533f); // ~±89°

			float speed = 5.0f;
			if (Input::IsKeyPressed(Key::LeftShift))
				speed *= 3.0f;

			float velocity = speed * ts;
			glm::vec3 forward = GetForwardDirection();
			glm::vec3 right = GetRightDirection();
			glm::vec3 up = { 0.0f, 1.0f, 0.0f };

			if (Input::IsKeyPressed(Key::W))
				m_FocalPoint += forward * velocity;
			if (Input::IsKeyPressed(Key::S))
				m_FocalPoint -= forward * velocity;
			if (Input::IsKeyPressed(Key::A))
				m_FocalPoint -= right * velocity;
			if (Input::IsKeyPressed(Key::D))
				m_FocalPoint += right * velocity;
			if (Input::IsKeyPressed(Key::Q))
				m_FocalPoint -= up * velocity;
			if (Input::IsKeyPressed(Key::E))
				m_FocalPoint += up * velocity;
		}
		else if (Input::IsKeyPressed(Key::LeftAlt))
		{
			if (Input::IsMouseButtonPressed(Mouse::ButtonMiddle))
				MousePan(delta);
			else if (Input::IsMouseButtonPressed(Mouse::ButtonLeft))
				MouseRotate(delta);
			else if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
				MouseZoom(delta.y);
		}
		else if (Input::IsMouseButtonPressed(Mouse::ButtonMiddle))
		{
			MousePan(delta);
		}

		if (m_Orthographic)
			m_Pitch = kTopPitch;

		UpdateView();
	}

	void EditorCamera::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<MouseScrolledEvent>(GE_BIND_EVENT_FN(EditorCamera::OnMouseScroll));
	}

	bool EditorCamera::OnMouseScroll(MouseScrolledEvent& e)
	{
		float delta = e.GetYOffset() * 0.1f;
		MouseZoom(delta);
		UpdateView();
		return false;
	}

	void EditorCamera::MousePan(const glm::vec2& delta)
	{
		auto [xSpeed, ySpeed] = PanSpeed();
		const float scale = m_Orthographic ? m_OrthoHeight : m_Distance;
		m_FocalPoint += -GetRightDirection() * delta.x * xSpeed * scale;
		m_FocalPoint += GetUpDirection() * delta.y * ySpeed * scale;
	}

	void EditorCamera::MouseRotate(const glm::vec2& delta)
	{
		float yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
		m_Yaw += yawSign * delta.x * RotationSpeed();
		if (!m_Orthographic)
			m_Pitch += delta.y * RotationSpeed();
	}

	void EditorCamera::MouseZoom(float delta)
	{
		if (m_Orthographic)
		{
			m_OrthoHeight -= delta * ZoomSpeed();
			m_OrthoHeight = glm::clamp(m_OrthoHeight, kMinOrthoHeight, kMaxOrthoHeight);
			UpdateProjection();
			return;
		}

		m_Distance -= delta * ZoomSpeed();
		if (m_Distance < 1.0f)
		{
			m_FocalPoint += GetForwardDirection();
			m_Distance = 1.0f;
		}
	}

	glm::vec3 EditorCamera::GetUpDirection() const
	{
		return GetOrientation() * glm::vec3(0.0f, 1.0f, 0.0f);
	}

	glm::vec3 EditorCamera::GetRightDirection() const
	{
		return GetOrientation() * glm::vec3(1.0f, 0.0f, 0.0f);
	}

	glm::vec3 EditorCamera::GetForwardDirection() const
	{
		return GetOrientation() * glm::vec3(0.0f, 0.0f, -1.0f);
	}

	glm::vec3 EditorCamera::CalculatePosition() const
	{
		return m_FocalPoint - GetForwardDirection() * m_Distance;
	}

	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}

	void EditorCamera::Frame(const glm::vec3& center, float radius)
	{
		m_FocalPoint = center;
		const float r = glm::max(radius, 0.05f);
		if (m_Orthographic)
		{
			m_OrthoHeight = glm::clamp(r * 2.2f, kMinOrthoHeight, kMaxOrthoHeight);
			m_Pitch = kTopPitch;
			UpdateProjection();
		}
		else
		{
			const float halfFov = glm::radians(m_FOV) * 0.5f;
			const float s = glm::sin(halfFov);
			m_Distance = (s > 1.0e-4f) ? (r / s) : 10.0f;
			m_Distance = glm::clamp(m_Distance, 1.0f, m_FarClip * 0.5f);
		}
		UpdateView();
	}
}
