#pragma once

#include "MeridianUIAPI/NifViewAPI.h"
#include "Render/NifCameraMath.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace Meridian::Render::NifPreview
{
    struct LightingRig
    {
        Float3 ambientTop{};
        Float3 ambientBottom{};
        std::array<Float3, 3> lightDirection{};
        std::array<Float3, 3> lightColor{};
        float exposureMultiplier = 1.0f;
    };

    namespace LightingDetail
    {
        struct Recipe
        {
            Float3 ambientTop;
            Float3 ambientBottom;
            std::array<Float3, 3> lightColor;
            float baseExposureStops;
        };

        inline Float3 Normalize(const Float3& a_value)
        {
            const auto length = std::sqrt(
                a_value.x * a_value.x + a_value.y * a_value.y + a_value.z * a_value.z);
            if (!std::isfinite(length) || length <= 0.00001f)
            {
                return {0.0f, 0.0f, 1.0f};
            }
            return {a_value.x / length, a_value.y / length, a_value.z / length};
        }

        inline Float3 CameraRelative(const CameraBasis& a_basis,
                                     float a_eye,
                                     float a_right,
                                     float a_up)
        {
            return Normalize({
                a_basis.eyeDirection.x * a_eye + a_basis.right.x * a_right +
                    a_basis.up.x * a_up,
                a_basis.eyeDirection.y * a_eye + a_basis.right.y * a_right +
                    a_basis.up.y * a_up,
                a_basis.eyeDirection.z * a_eye + a_basis.right.z * a_right +
                    a_basis.up.z * a_up,
            });
        }

        inline Recipe SelectRecipe(Meridian::UI::NifView::LightingPreset a_preset)
        {
            using Meridian::UI::NifView::LightingPreset;
            switch (a_preset)
            {
            case LightingPreset::Bright:
                // Aetherius: AetheriusUIBridge utiliza LightingPreset::Bright na previa do
                // inventario. Calibrado em espaco linear para manter metais escuros (Adaga de
                // Ferro, Elmo Daedrico) e madeira (Sopas) perfeitamente legiveis, sem lavar
                // texturas claras (rolha/corda) nem introduzir dominante azulada.
                return {
                    .ambientTop = {0.34f, 0.33f, 0.31f},
                    .ambientBottom = {0.16f, 0.155f, 0.15f},
                    .lightColor = {{{0.92f, 0.87f, 0.80f},
                                    {0.38f, 0.38f, 0.39f},
                                    {0.30f, 0.30f, 0.32f}}},
                    .baseExposureStops = 0.0f,
                };
            case LightingPreset::Dramatic:
                return {
                    .ambientTop = {0.12f, 0.12f, 0.14f},
                    .ambientBottom = {0.025f, 0.028f, 0.038f},
                    .lightColor = {{{1.12f, 0.96f, 0.78f},
                                    {0.12f, 0.14f, 0.20f},
                                    {0.52f, 0.58f, 0.70f}}},
                    .baseExposureStops = 0.05f,
                };
            case LightingPreset::Neutral:
            default:
                return {
                    .ambientTop = {0.28f, 0.27f, 0.26f},
                    .ambientBottom = {0.12f, 0.12f, 0.12f},
                    .lightColor = {{{0.84f, 0.80f, 0.74f},
                                    {0.32f, 0.32f, 0.33f},
                                    {0.24f, 0.24f, 0.26f}}},
                    .baseExposureStops = -0.08f,
                };
            }
        }
    }

    inline LightingRig BuildLightingRig(
        Meridian::UI::NifView::LightingPreset a_preset,
        float a_exposureStops,
        const CameraBasis& a_basis)
    {
        if (!Meridian::UI::NifView::IsValidLightingPreset(a_preset))
        {
            a_preset = Meridian::UI::NifView::LightingPreset::Neutral;
        }
        const auto recipe = LightingDetail::SelectRecipe(a_preset);
        const auto exposureStops = std::clamp(
            std::isfinite(a_exposureStops) ? a_exposureStops : 0.0f, -2.0f, 2.0f);

        return {
            .ambientTop = recipe.ambientTop,
            .ambientBottom = recipe.ambientBottom,
            .lightDirection = {{
                LightingDetail::CameraRelative(a_basis, 0.72f, -0.46f, 0.52f),
                LightingDetail::CameraRelative(a_basis, 0.52f, 0.78f, 0.12f),
                LightingDetail::CameraRelative(a_basis, -0.58f, -0.18f, 0.76f),
            }},
            .lightColor = recipe.lightColor,
            .exposureMultiplier = std::exp2(recipe.baseExposureStops + exposureStops),
        };
    }
}
