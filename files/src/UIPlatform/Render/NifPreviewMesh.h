#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Meridian::Render::NifPreview
{
    enum class MaterialTintKind : std::uint8_t
    {
        None,
        Body,
        Hair,
    };

    enum class PreviewMaterialFeature : std::uint8_t
    {
        Generic,
        FaceGen,
        FaceGenRgbTint,
        HairTint,
        Effect,
    };

    struct Float2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Float4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    inline Float3 operator+(Float3 a_left, Float3 a_right)
    {
        return {a_left.x + a_right.x, a_left.y + a_right.y, a_left.z + a_right.z};
    }

    inline Float3 operator-(Float3 a_left, Float3 a_right)
    {
        return {a_left.x - a_right.x, a_left.y - a_right.y, a_left.z - a_right.z};
    }

    inline Float3& operator+=(Float3& a_left, Float3 a_right)
    {
        a_left = a_left + a_right;
        return a_left;
    }

    inline Float3 Cross(Float3 a_left, Float3 a_right)
    {
        return {
            a_left.y * a_right.z - a_left.z * a_right.y,
            a_left.z * a_right.x - a_left.x * a_right.z,
            a_left.x * a_right.y - a_left.y * a_right.x,
        };
    }

    inline Float3 Normalize(Float3 a_value)
    {
        const auto lengthSquared =
            a_value.x * a_value.x + a_value.y * a_value.y + a_value.z * a_value.z;
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f)
        {
            return {0.0f, 0.0f, 1.0f};
        }
        const auto inverseLength = 1.0f / std::sqrt(lengthSquared);
        return {a_value.x * inverseLength, a_value.y * inverseLength, a_value.z * inverseLength};
    }

    struct PreviewVertex
    {
        Float3 position{};
        Float3 normal{0.0f, 0.0f, 1.0f};
        Float4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
        Float2 textureCoordinate{};
        Float4 color{1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> boneWeights{};
        std::array<std::uint16_t, 4> boneIndices{};
    };

    enum class PreviewAnimationTarget : std::uint8_t
    {
        UOffset = 0,
        VOffset,
        UScale,
        VScale,
        Alpha,
        EmissiveMultiple,
        SpecularStrength,
        EnvironmentScale,
        Glossiness,
        FalloffStartAngle,
        FalloffStopAngle,
        FalloffStartOpacity,
        FalloffStopOpacity,
    };

    enum class PreviewAnimationCycle : std::uint8_t
    {
        Loop = 0,
        Reverse,
        Clamp,
    };

    struct PreviewFloatKey
    {
        float time = 0.0f;
        float value = 0.0f;
    };

    struct PreviewFloatTrack
    {
        PreviewAnimationTarget target = PreviewAnimationTarget::VOffset;
        PreviewAnimationCycle cycle = PreviewAnimationCycle::Loop;
        float frequency = 1.0f;
        float phase = 0.0f;
        float startTime = 0.0f;
        float stopTime = 0.0f;
        std::vector<PreviewFloatKey> keys;
    };

    inline bool EvaluateFloatTrack(const PreviewFloatTrack& a_track,
                                   float a_elapsedSeconds,
                                   float& a_outValue)
    {
        if (a_track.keys.empty())
        {
            return false;
        }
        if (a_track.keys.size() == 1)
        {
            const float v = a_track.keys.front().value;
            if (!std::isfinite(v))
            {
                return false;
            }
            a_outValue = v;
            return true;
        }

        const float freq = std::isfinite(a_track.frequency) ? a_track.frequency : 1.0f;
        const float phase = std::isfinite(a_track.phase) ? a_track.phase : 0.0f;
        const float rawTime = a_elapsedSeconds * freq + phase;
        const float start = std::isfinite(a_track.startTime) ? a_track.startTime : a_track.keys.front().time;
        const float stop = (std::isfinite(a_track.stopTime) && a_track.stopTime > start)
            ? a_track.stopTime
            : a_track.keys.back().time;
        const float duration = stop - start;

        float sampleTime = rawTime;
        if (duration > 1.0e-5f)
        {
            if (a_track.cycle == PreviewAnimationCycle::Loop)
            {
                float wrapped = std::fmod(rawTime - start, duration);
                if (wrapped < 0.0f)
                {
                    wrapped += duration;
                }
                sampleTime = start + wrapped;
            }
            else if (a_track.cycle == PreviewAnimationCycle::Reverse)
            {
                const float period = duration * 2.0f;
                float wrapped = std::fmod(rawTime - start, period);
                if (wrapped < 0.0f)
                {
                    wrapped += period;
                }
                sampleTime = wrapped <= duration ? (start + wrapped) : (stop - (wrapped - duration));
            }
            else
            {
                sampleTime = std::clamp(rawTime, start, stop);
            }
        }
        else
        {
            sampleTime = start;
        }

        if (sampleTime <= a_track.keys.front().time)
        {
            a_outValue = a_track.keys.front().value;
            return std::isfinite(a_outValue);
        }
        if (sampleTime >= a_track.keys.back().time)
        {
            a_outValue = a_track.keys.back().value;
            return std::isfinite(a_outValue);
        }

        for (std::size_t i = 0; i + 1 < a_track.keys.size(); ++i)
        {
            const auto& k0 = a_track.keys[i];
            const auto& k1 = a_track.keys[i + 1];
            if (sampleTime <= k1.time)
            {
                const float span = k1.time - k0.time;
                const float t = span > 1.0e-6f ? std::clamp((sampleTime - k0.time) / span, 0.0f, 1.0f) : 0.0f;
                a_outValue = k0.value + (k1.value - k0.value) * t;
                return std::isfinite(a_outValue);
            }
        }

        a_outValue = a_track.keys.back().value;
        return std::isfinite(a_outValue);
    }

    struct PreviewMaterial
    {
        std::string diffuseTexturePath;
        std::string normalTexturePath;
        std::string specularTexturePath;
        std::string environmentTexturePath;
        std::string environmentMaskTexturePath;
        std::string glowTexturePath;
        std::string faceTintTexturePath;
        std::string faceDetailTexturePath;
        std::string faceSubsurfaceTexturePath;
        Float2 textureOffset{};
        Float2 textureScale{1.0f, 1.0f};
        Float3 specularColor{1.0f, 1.0f, 1.0f};
        Float3 tintColor{1.0f, 1.0f, 1.0f};
        Float3 emissiveColor{0.0f, 0.0f, 0.0f};
        float opacity = 1.0f;
        float alphaThreshold = 0.0f;
        float specularPower = 1.0f;
        float specularStrength = 1.0f;
        float environmentScale = 0.0f;
        float emissiveMultiple = 0.0f;
        float falloffStartAngle = 1.0f;
        float falloffStopAngle = 0.0f;
        float falloffStartOpacity = 1.0f;
        float falloffStopOpacity = 1.0f;
        bool alphaBlend = false;
        bool alphaTest = false;
        bool additiveBlend = false;
        bool premultipliedAlpha = false;
        bool specularEnabled = false;
        bool tangentSpaceNormal = false;
        bool modelSpaceNormal = false;
        bool environmentEnabled = false;
        bool envmapLightFade = false;
        bool vertexColors = false;
        bool vertexAlpha = false;
        bool ownEmit = false;
        bool glowMapEnabled = false;
        bool effectShader = false;
        bool effectLighting = false;
        bool grayscaleToPaletteColor = false;
        bool grayscaleToPaletteAlpha = false;
        bool falloffEnabled = false;
        PreviewMaterialFeature feature = PreviewMaterialFeature::Generic;
        MaterialTintKind tintKind = MaterialTintKind::None;
        // Unsupported shaders keep the historical no-cull behavior so a
        // missing material never makes otherwise valid geometry disappear.
        bool twoSided = true;
        std::vector<PreviewFloatTrack> floatTracks;
    };

    struct PreviewDrawRange
    {
        std::uint64_t sceneObject = 0;
        std::uint32_t startIndex = 0;
        std::uint32_t indexCount = 0;
        PreviewMaterial material{};
    };

    struct PreviewBounds
    {
        Float3 minimum{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
        };
        Float3 maximum{
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
        };
        bool valid = false;

        void Include(Float3 a_point)
        {
            minimum.x = std::min(minimum.x, a_point.x);
            minimum.y = std::min(minimum.y, a_point.y);
            minimum.z = std::min(minimum.z, a_point.z);
            maximum.x = std::max(maximum.x, a_point.x);
            maximum.y = std::max(maximum.y, a_point.y);
            maximum.z = std::max(maximum.z, a_point.z);
            valid = true;
        }

        void Include(const PreviewBounds& a_other)
        {
            if (!a_other.valid)
            {
                return;
            }
            Include(a_other.minimum);
            Include(a_other.maximum);
        }
    };

    struct PreviewInventoryMarker
    {
        bool present = false;
        Float3 rotationRadians{};
        float zoom = 1.0f;
    };

    struct PreviewMesh
    {
        std::vector<PreviewVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<PreviewDrawRange> draws;
        PreviewBounds bounds{};
        PreviewInventoryMarker inventoryMarker{};

        bool Empty() const
        {
            return vertices.empty() || indices.empty() || !bounds.valid;
        }
    };
}
