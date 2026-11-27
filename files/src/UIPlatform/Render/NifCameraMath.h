#pragma once

#include "Render/NifPreviewMesh.h"

#include <cmath>

namespace Meridian::Render::NifPreview
{
    struct CameraBasis
    {
        Float3 eyeDirection{};
        Float3 right{};
        Float3 up{};
    };

    inline CameraBasis BuildCameraBasis(float a_yawRadians, float a_pitchRadians)
    {
        const auto cosYaw = std::cos(a_yawRadians);
        const auto sinYaw = std::sin(a_yawRadians);
        const auto cosPitch = std::cos(a_pitchRadians);
        const auto sinPitch = std::sin(a_pitchRadians);
        return {
            .eyeDirection = {cosPitch * cosYaw, cosPitch * sinYaw, sinPitch},
            .right = {-sinYaw, cosYaw, 0.0f},
            .up = {-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch},
        };
    }

    // Aetherius: referencial da prévia tirado da caixa do modelo, para o item já abrir
    // reconhecível. Com yaw/pitch zero a câmera olha a face larga (perpendicular ao eixo
    // mais fino) e o eixo mais comprido fica na vertical da tela: couraça "deitada" (GND)
    // vista de frente, espada vista de lado. Modelo quase cúbico fica no mundo (Z para cima).
    inline CameraBasis OrientToModel(const CameraBasis& a_basis, const Float3& a_extent)
    {
        const float extents[3] = {a_extent.x, a_extent.y, a_extent.z};
        int thinnest = 0;
        int longest = 0;
        for (int index = 1; index < 3; ++index)
        {
            if (extents[index] < extents[thinnest]) thinnest = index;
            if (extents[index] > extents[longest]) longest = index;
        }
        if (thinnest == longest || extents[longest] < extents[thinnest] * 1.6f)
        {
            return a_basis;
        }
        const auto axis = [](int a_index) {
            return Float3{a_index == 0 ? 1.0f : 0.0f, a_index == 1 ? 1.0f : 0.0f, a_index == 2 ? 1.0f : 0.0f};
        };
        const auto frameX = axis(thinnest);
        const auto frameZ = axis(longest);
        const Float3 frameY{frameZ.y * frameX.z - frameZ.z * frameX.y,
                            frameZ.z * frameX.x - frameZ.x * frameX.z,
                            frameZ.x * frameX.y - frameZ.y * frameX.x};
        const auto map = [&](const Float3& a_vector) {
            return Float3{frameX.x * a_vector.x + frameY.x * a_vector.y + frameZ.x * a_vector.z,
                          frameX.y * a_vector.x + frameY.y * a_vector.y + frameZ.y * a_vector.z,
                          frameX.z * a_vector.x + frameY.z * a_vector.y + frameZ.z * a_vector.z};
        };
        return {map(a_basis.eyeDirection), map(a_basis.right), map(a_basis.up)};
    }

    struct Quaternion
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    inline Quaternion NormalizeQuaternion(const Quaternion& a_q)
    {
        const float lenSq = a_q.x * a_q.x + a_q.y * a_q.y + a_q.z * a_q.z + a_q.w * a_q.w;
        if (!std::isfinite(lenSq) || lenSq <= 1.0e-12f)
        {
            return {};
        }
        const float invLen = 1.0f / std::sqrt(lenSq);
        return {a_q.x * invLen, a_q.y * invLen, a_q.z * invLen, a_q.w * invLen};
    }

    inline Quaternion MultiplyQuaternion(const Quaternion& a_left, const Quaternion& a_right)
    {
        return {
            a_left.w * a_right.x + a_left.x * a_right.w + a_left.y * a_right.z - a_left.z * a_right.y,
            a_left.w * a_right.y - a_left.x * a_right.z + a_left.y * a_right.w + a_left.z * a_right.x,
            a_left.w * a_right.z + a_left.x * a_right.y - a_left.y * a_right.x + a_left.z * a_right.w,
            a_left.w * a_right.w - a_left.x * a_right.x - a_left.y * a_right.y - a_left.z * a_right.z,
        };
    }

    inline Quaternion ConjugateQuaternion(const Quaternion& a_q)
    {
        return {-a_q.x, -a_q.y, -a_q.z, a_q.w};
    }

    inline Quaternion QuaternionFromAxisAngle(const Float3& a_axis, float a_radians)
    {
        const auto n = Normalize(a_axis);
        const float half = a_radians * 0.5f;
        const float s = std::sin(half);
        const float c = std::cos(half);
        return {n.x * s, n.y * s, n.z * s, c};
    }

    inline Float3 RotateVector(const Quaternion& a_q, const Float3& a_v)
    {
        const Float3 u{a_q.x, a_q.y, a_q.z};
        const Float3 t = Cross(u, a_v);
        const Float3 t2{t.x * 2.0f, t.y * 2.0f, t.z * 2.0f};
        const Float3 c = Cross(u, t2);
        return {
            a_v.x + a_q.w * t2.x + c.x,
            a_v.y + a_q.w * t2.y + c.y,
            a_v.z + a_q.w * t2.z + c.z,
        };
    }

    // Aetherius: reproduz exatamente RE::NiMatrix3::SetEulerAnglesXYZ(x, y, z)
    // usado pelo BSInvMarker do Inventory3DManager do Skyrim SE.
    // Em NetImmerse/Skyrim (NiMatrix3.cpp:105-172), MakeX/Y/ZRotation aplicam
    // rotacoes clockwise/transpostas: SetEulerAnglesXYZ(x,y,z) = Rx_RH(-x) * Ry_RH(-y) * Rz_RH(-z).
    inline Quaternion BuildInvMarkerQuaternion(const Float3& a_eulerXYZRadians)
    {
        const auto qx = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, -a_eulerXYZRadians.x);
        const auto qy = QuaternionFromAxisAngle({0.0f, 1.0f, 0.0f}, -a_eulerXYZRadians.y);
        const auto qz = QuaternionFromAxisAngle({0.0f, 0.0f, 1.0f}, -a_eulerXYZRadians.z);
        return NormalizeQuaternion(MultiplyQuaternion(MultiplyQuaternion(qx, qy), qz));
    }

    // Orientacao natural para itens de mods sem BSInvMarker: mantem +Z para cima
    // com leve inclinacao estetica (12 deg em X, 15 deg em Z), sem tombar o modelo.
    inline Quaternion BuildDefaultInventoryQuaternion()
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        return BuildInvMarkerQuaternion({12.0f * kDegToRad, 0.0f, 15.0f * kDegToRad});
    }

    // Rotacao livre 360 graus em espaco de tela (mesmo sentido do Skyrim vanilla,
    // com camera em +Y olhando -Y, direita = -X, cima = +Z):
    // +deltaX (mouse para direita) gira a frente (+Y) para a direita (-X) via Rz(+deltaX);
    // +deltaY (mouse para baixo) gira a frente (+Y) para baixo (-Z) e o topo (+Z) para +Y via Rx(-deltaY).
    inline Quaternion ApplyScreenTrackballDelta(const Quaternion& a_current,
                                                float a_deltaXRadians,
                                                float a_deltaYRadians)
    {
        const auto qYaw = QuaternionFromAxisAngle({0.0f, 0.0f, 1.0f}, a_deltaXRadians);
        const auto qPitch = QuaternionFromAxisAngle({-1.0f, 0.0f, 0.0f}, a_deltaYRadians);
        const auto qDelta = MultiplyQuaternion(qPitch, qYaw);
        return NormalizeQuaternion(MultiplyQuaternion(qDelta, a_current));
    }

    // Converte a orientacao 3D do item no referencial oficial do Inventory3DManager
    // do Skyrim (camera em +Y olhando para -Y, -X direita, +Z cima) para a CameraBasis
    // equivalente, mantendo as luzes de estudio fixas em relacao a tela.
    inline CameraBasis CameraBasisFromObjectOrientation(const Quaternion& a_objectOrientation)
    {
        const auto inv = ConjugateQuaternion(NormalizeQuaternion(a_objectOrientation));
        return {
            .eyeDirection = Normalize(RotateVector(inv, {0.0f, 1.0f, 0.0f})),
            .right = Normalize(RotateVector(inv, {-1.0f, 0.0f, 0.0f})),
            .up = Normalize(RotateVector(inv, {0.0f, 0.0f, 1.0f})),
        };
    }

    inline Float3 ApplyNormalizedPan(const Float3& a_center,
                                     float a_radius,
                                     const CameraBasis& a_basis,
                                     float a_panX,
                                     float a_panY,
                                     float a_panZ)
    {
        return {
            a_center.x + a_radius * (a_basis.right.x * a_panX +
                                     a_basis.up.x * a_panY +
                                     a_basis.eyeDirection.x * a_panZ),
            a_center.y + a_radius * (a_basis.right.y * a_panX +
                                     a_basis.up.y * a_panY +
                                     a_basis.eyeDirection.y * a_panZ),
            a_center.z + a_radius * (a_basis.right.z * a_panX +
                                     a_basis.up.z * a_panY +
                                     a_basis.eyeDirection.z * a_panZ),
        };
    }
}
