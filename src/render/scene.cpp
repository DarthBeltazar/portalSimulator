#include "scene.hpp"

namespace render {

Scene::Scene() {
    device_ = rtcNewDevice(nullptr);
    scene_ = rtcNewScene(device_);
}

Scene::~Scene() {
    if (scene_ != nullptr) {
        rtcReleaseScene(scene_);
    }
    if (device_ != nullptr) {
        rtcReleaseDevice(device_);
    }
}

void Scene::addTriangleMesh(const TriangleMesh& mesh) {
    RTCGeometry geometry = rtcNewGeometry(device_, RTC_GEOMETRY_TYPE_TRIANGLE);

    auto* vertices = static_cast<float*>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), mesh.vertices.size()));
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Eigen::Vector3d& v = mesh.vertices[i];
        vertices[3 * i + 0] = static_cast<float>(v.x());
        vertices[3 * i + 1] = static_cast<float>(v.y());
        vertices[3 * i + 2] = static_cast<float>(v.z());
    }

    auto* indices = static_cast<unsigned int*>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned int), mesh.triangles.size()));
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        indices[3 * i + 0] = mesh.triangles[i][0];
        indices[3 * i + 1] = mesh.triangles[i][1];
        indices[3 * i + 2] = mesh.triangles[i][2];
    }

    rtcCommitGeometry(geometry);
    rtcAttachGeometry(scene_, geometry);
    rtcReleaseGeometry(geometry); // the scene now owns the only remaining reference

    meshes_.push_back(mesh);
}

void Scene::addPortal(manifold::Portal portal) { portals_.push_back(std::move(portal)); }

void Scene::addLight(PointLight light) { lights_.push_back(light); }

void Scene::commit() { rtcCommitScene(scene_); }

} // namespace render
