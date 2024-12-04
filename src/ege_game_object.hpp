#pragma once


#include "ege_model.hpp"

#include<glm/gtc/matrix_transform.hpp>

#include <memory>
#include <unordered_map>


namespace ege {

	struct TransformComponent {
		glm::vec3 translation{};
		glm::vec3 scale {1.f, 1.f, 1.f};
		glm::vec3 rotation{};

		glm::mat4 mat4();
		glm::mat3 normalMatrix();
	};

	class EgeGameObject {
	public:

		using id_t = unsigned int;
		using Map = std::unordered_map<id_t, EgeGameObject>;

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
		TransformComponent transform{ };

	private:

		EgeGameObject(id_t objId): id{objId}{}
		id_t id;


	};
}