#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace veekay::scene {

class Camera {
public:
    Camera(const glm::vec3& position = glm::vec3(0.0f, 0.0f, 5.0f))
        : position_(position)
        , front_(0.0f, 0.0f, -1.0f)
        , up_(0.0f, 1.0f, 0.0f)
        , yaw_(-90.0f)
        , pitch_(0.0f)
    {}

    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position_, position_ + front_, up_);
    }

    glm::vec3 getPosition() const { return position_; }
    glm::vec3 getFront() const { return front_; }

    void setPosition(const glm::vec3& pos) { position_ = pos; }

    // Для будущего: движение камеры
    void moveForward(float distance) { position_ += front_ * distance; }
    void moveRight(float distance) { 
        position_ += glm::normalize(glm::cross(front_, up_)) * distance; 
    }
    void moveUp(float distance) { position_ += up_ * distance; }

    void rotate(float yaw_offset, float pitch_offset) {
        yaw_ += yaw_offset;
        pitch_ += pitch_offset;
        
        if (pitch_ > 89.0f) pitch_ = 89.0f;
        if (pitch_ < -89.0f) pitch_ = -89.0f;

        updateVectors();
    }

private:
    void updateVectors() {
        glm::vec3 direction;
        direction.x = cos(glm::radians(yaw_)) * cos(glm::radians(pitch_));
        direction.y = sin(glm::radians(pitch_));
        direction.z = sin(glm::radians(yaw_)) * cos(glm::radians(pitch_));
        front_ = glm::normalize(direction);
    }

    glm::vec3 position_;
    glm::vec3 front_;
    glm::vec3 up_;
    float yaw_;
    float pitch_;
};

} // namespace veekay::scene