#pragma once


#include "ege_model.hpp"

#include <memory>

namespace ege {

	struct Transform2dComponent {
		glm::vec2 translation{};
		glm::vec2 scale {1.f, 1.f};
		float rotation;

		glm::mat2 mat2() { 
			const float sinTheta = glm::sin(rotation);
			const float cosTheta = glm::cos(rotation);

			glm::mat2 rotMatrix{ {cosTheta, sinTheta} , {-sinTheta, cosTheta} };
			// the constructor gets columns as input and not rows.
			glm::mat2 scaleMat{ {scale.x, 0.0f}, {0.0f, scale.y}};
			return rotMatrix * scaleMat;
		}
	};

	class EgeGameObject {
	public:

		using id_t = unsigned int;

		static EgeGameObject createGameObject() {
			static id_t currentId = 0;

			return EgeGameObject{ currentId++ };
		}

		EgeGameObject(const EgeGameObject&) = delete;
		EgeGameObject& operator=(const EgeGameObject&) = delete;
		EgeGameObject(EgeGameObject&&) = default;
		EgeGameObject& operator=(EgeGameObject&&) = default;



		id_t getId() { return id; }

		std::shared_ptr<EgeModel> model{};
		glm::vec3 color{};
		Transform2dComponent transform2d{ };

	private:

		EgeGameObject(id_t objId): id{objId}{}
		id_t id;


	};
}