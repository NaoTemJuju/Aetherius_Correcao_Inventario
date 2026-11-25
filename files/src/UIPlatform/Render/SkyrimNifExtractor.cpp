#include "Render/SkyrimNifExtractor.h"

#include "Render/BindPoseSkinning.h"
#include "Render/NifTextureOverride.h"
#include "Render/NifTexturePath.h"
#include "Render/PackedGeometryDecoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

namespace Meridian::Render::NifPreview
{
    namespace
    {
        inline constexpr std::size_t MAX_MODEL_NODES = 100'000;
        inline constexpr std::size_t MAX_MODEL_DEPTH = 1'024;
        inline constexpr std::size_t MAX_SKIN_BONES = 512;
        inline constexpr std::size_t MAX_SKIN_PARTITIONS = 256;
        inline constexpr std::size_t MAX_PARTITION_BONES = 256;

        using ObjectTransformMap = std::unordered_map<const RE::NiAVObject*, AffineTransform>;

        AffineTransform ToAffineTransform(const RE::NiTransform& a_transform)
        {
            AffineTransform result{};
            for (std::size_t row = 0; row < 3; ++row)
            {
                for (std::size_t column = 0; column < 3; ++column)
                {
                    result.rotation[row][column] = a_transform.rotate.entry[row][column];
                }
            }
            result.translation = {
                a_transform.translate.x,
                a_transform.translate.y,
                a_transform.translate.z,
            };
            result.scale = a_transform.scale;
            return result;
        }

        bool CollectObjectTransforms(RE::NiAVObject* a_object,
                                     const AffineTransform& a_parent,
                                     ObjectTransformMap& a_transforms,
                                     std::size_t a_depth)
        {
            if (a_object == nullptr)
            {
                return true;
            }
            if (a_depth > MAX_MODEL_DEPTH || a_transforms.size() >= MAX_MODEL_NODES)
            {
                return false;
            }
            if (a_transforms.contains(a_object))
            {
                return true;
            }

            const auto modelTransform = SkinningDetail::Compose(
                a_parent, ToAffineTransform(a_object->local));
            if (!SkinningDetail::IsFinite(modelTransform))
            {
                return false;
            }
            a_transforms.emplace(a_object, modelTransform);
            if (auto* node = a_object->AsNode(); node != nullptr)
            {
                for (const auto& child : node->GetChildren())
                {
                    if (child != nullptr &&
                        !CollectObjectTransforms(
                            child.get(), modelTransform, a_transforms, a_depth + 1))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool CollectLiveObjectTransforms(RE::NiAVObject* a_object,
                                         const AffineTransform& a_actorWorld,
                                         ObjectTransformMap& a_transforms,
                                         std::size_t a_depth)
        {
            if (a_object == nullptr)
            {
                return true;
            }
            if (a_depth > MAX_MODEL_DEPTH || a_transforms.size() >= MAX_MODEL_NODES)
            {
                return false;
            }
            if (a_transforms.contains(a_object))
            {
                return true;
            }

            AffineTransform actorRelative{};
            if (!TryMakeRelativeTransform(
                    a_actorWorld, ToAffineTransform(a_object->world), actorRelative))
            {
                return false;
            }
            a_transforms.emplace(a_object, actorRelative);
            if (auto* node = a_object->AsNode(); node != nullptr)
            {
                for (const auto& child : node->GetChildren())
                {
                    if (child != nullptr &&
                        !CollectLiveObjectTransforms(
                            child.get(), a_actorWorld, a_transforms, a_depth + 1))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool CopyGpuBuffer(ID3D11Device* a_device,
                           ID3D11DeviceContext* a_context,
                           ID3D11Buffer* a_source,
                           std::size_t a_requiredBytes,
                           std::vector<std::uint8_t>& a_output)
        {
            if (a_device == nullptr || a_context == nullptr || a_source == nullptr || a_requiredBytes == 0)
            {
                return false;
            }

            D3D11_BUFFER_DESC sourceDesc{};
            a_source->GetDesc(&sourceDesc);
            if (a_requiredBytes > sourceDesc.ByteWidth)
            {
                return false;
            }

            D3D11_BUFFER_DESC stagingDesc = sourceDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            stagingDesc.StructureByteStride = 0;

            Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
            if (FAILED(a_device->CreateBuffer(&stagingDesc, nullptr, staging.GetAddressOf())))
            {
                return false;
            }

            a_context->CopyResource(staging.Get(), a_source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(a_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                return false;
            }

            a_output.resize(a_requiredBytes);
            std::memcpy(a_output.data(), mapped.pData, a_requiredBytes);
            a_context->Unmap(staging.Get(), 0);
            return true;
        }

        enum class ShapeResult
        {
            Accepted,
            Unsupported,
            Malformed,
            ReadbackFailed,
        };

        bool ContainsInsensitive(std::string_view a_haystack, std::string_view a_needle)
        {
            if (a_needle.empty() || a_haystack.size() < a_needle.size())
            {
                return false;
            }
            const auto it = std::search(
                a_haystack.begin(),
                a_haystack.end(),
                a_needle.begin(),
                a_needle.end(),
                [](char a_left, char a_right) {
                    return std::tolower(static_cast<unsigned char>(a_left)) ==
                           std::tolower(static_cast<unsigned char>(a_right));
                });
            return it != a_haystack.end();
        }

        bool IsWorldHelperNodeName(std::string_view a_name)
        {
            if (a_name.empty())
            {
                return false;
            }
            return ContainsInsensitive(a_name, "editormarker") ||
                   ContainsInsensitive(a_name, "pplight") ||
                   ContainsInsensitive(a_name, "heatrefraction");
        }

        bool IsEnbLightDummyTexture(std::string_view a_path)
        {
            if (a_path.empty())
            {
                return false;
            }
            return ContainsInsensitive(a_path, "pp_black.dds") ||
                   ContainsInsensitive(a_path, "enblight") ||
                   ContainsInsensitive(a_path, "enblightglow");
        }

        RE::BSEffectShaderProperty* CastEffectShaderProperty(RE::NiProperty* a_property)
        {
            if (a_property == nullptr)
            {
                return nullptr;
            }
            const auto* rtti = a_property->GetRTTI();
            if (rtti == nullptr || rtti->GetName() == nullptr)
            {
                return nullptr;
            }
            if (std::string_view(rtti->GetName()) == "BSEffectShaderProperty")
            {
                return static_cast<RE::BSEffectShaderProperty*>(a_property);
            }
            return nullptr;
        }

        void ReadAlphaProperty(RE::NiAlphaProperty* a_alpha, PreviewMaterial& a_material)
        {
            if (a_alpha == nullptr)
            {
                return;
            }
            a_material.alphaBlend = a_alpha->GetAlphaBlending();
            a_material.alphaTest = a_alpha->GetAlphaTesting();
            a_material.alphaThreshold = static_cast<float>(a_alpha->alphaThreshold) / 255.0f;
            a_material.additiveBlend =
                a_material.alphaBlend &&
                a_alpha->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kOne;
        }

        bool MapLightingControlledVariable(std::uint32_t a_varId, PreviewAnimationTarget& a_target)
        {
            switch (a_varId)
            {
            case 8:  a_target = PreviewAnimationTarget::EnvironmentScale; return true;
            case 9:  a_target = PreviewAnimationTarget::Glossiness; return true;
            case 10: a_target = PreviewAnimationTarget::SpecularStrength; return true;
            case 11: a_target = PreviewAnimationTarget::EmissiveMultiple; return true;
            case 12: a_target = PreviewAnimationTarget::Alpha; return true;
            case 20: a_target = PreviewAnimationTarget::UOffset; return true;
            case 21: a_target = PreviewAnimationTarget::UScale; return true;
            case 22: a_target = PreviewAnimationTarget::VOffset; return true;
            case 23: a_target = PreviewAnimationTarget::VScale; return true;
            default: return false;
            }
        }

        bool MapEffectControlledVariable(std::uint32_t a_varId, PreviewAnimationTarget& a_target)
        {
            switch (a_varId)
            {
            case 0: a_target = PreviewAnimationTarget::EmissiveMultiple; return true;
            case 1: a_target = PreviewAnimationTarget::FalloffStartAngle; return true;
            case 2: a_target = PreviewAnimationTarget::FalloffStopAngle; return true;
            case 3: a_target = PreviewAnimationTarget::FalloffStartOpacity; return true;
            case 4: a_target = PreviewAnimationTarget::FalloffStopOpacity; return true;
            case 5: a_target = PreviewAnimationTarget::Alpha; return true;
            case 6: a_target = PreviewAnimationTarget::UOffset; return true;
            case 7: a_target = PreviewAnimationTarget::UScale; return true;
            case 8: a_target = PreviewAnimationTarget::VOffset; return true;
            case 9: a_target = PreviewAnimationTarget::VScale; return true;
            default: return false;
            }
        }

        void ExtractShaderFloatControllers(RE::NiObjectNET* a_owner,
                                           bool a_isEffectShader,
                                           PreviewMaterial& a_material)
        {
            if (a_owner == nullptr)
            {
                return;
            }
            constexpr std::size_t kMaxControllersPerShader = 16;
            constexpr std::uint32_t kMaxKeysPerTrack = 512;
            std::size_t visited = 0;

            for (auto* ctrl = a_owner->GetControllers();
                 ctrl != nullptr && visited < kMaxControllersPerShader;
                 ctrl = ctrl->GetNext(), ++visited)
            {
                const auto* rtti = ctrl->GetRTTI();
                if (rtti == nullptr || rtti->GetName() == nullptr)
                {
                    continue;
                }
                const std::string_view typeName(rtti->GetName());
                const bool isLightingFloat = typeName == "BSLightingShaderPropertyFloatController";
                const bool isEffectFloat = typeName == "BSEffectShaderPropertyFloatController";
                const bool isAlphaCtrl = typeName == "NiAlphaController";
                if (!isLightingFloat && !isEffectFloat && !isAlphaCtrl)
                {
                    continue;
                }

                auto* interpCtrl = static_cast<RE::NiInterpController*>(ctrl);
                auto* rawInterp = interpCtrl->GetInterpolator(0);
                if (rawInterp == nullptr)
                {
                    continue;
                }
                const auto* interpRtti = rawInterp->GetRTTI();
                if (interpRtti == nullptr || interpRtti->GetName() == nullptr ||
                    std::string_view(interpRtti->GetName()) != "NiFloatInterpolator")
                {
                    continue;
                }

                auto* floatInterp = static_cast<RE::NiFloatInterpolator*>(rawInterp);
                const auto* floatData = floatInterp->floatData.get();
                if (floatData == nullptr || floatData->numKeys < 2 ||
                    floatData->numKeys > kMaxKeysPerTrack || floatData->keys == nullptr ||
                    floatData->keySize < sizeof(RE::NiFloatKey))
                {
                    continue;
                }

                PreviewAnimationTarget target = PreviewAnimationTarget::Alpha;
                if (!isAlphaCtrl)
                {
                    // Validacao de layout em tempo de execucao: em NiSingleInterpController,
                    // o campo interpolator (NiPointer<NiInterpolator>) fica logo apos
                    // NiInterpController (0x48), e controlledVariable (uint32) logo apos (0x50).
                    const auto* rawBytes = reinterpret_cast<const std::uint8_t*>(ctrl);
                    const auto* storedInterp = *reinterpret_cast<const RE::NiInterpolator* const*>(
                        rawBytes + sizeof(RE::NiInterpController));
                    if (storedInterp != rawInterp)
                    {
                        continue;
                    }
                    std::uint32_t varId = 0;
                    std::memcpy(
                        &varId,
                        rawBytes + sizeof(RE::NiInterpController) + sizeof(void*),
                        sizeof(varId));
                    const bool mapped = (isEffectFloat || a_isEffectShader)
                        ? MapEffectControlledVariable(varId, target)
                        : MapLightingControlledVariable(varId, target);
                    if (!mapped)
                    {
                        continue;
                    }
                }

                PreviewFloatTrack track{};
                track.target = target;
                const auto rawCycle =
                    (static_cast<std::uint16_t>(ctrl->flags.underlying()) &
                     static_cast<std::uint16_t>(RE::NiTimeController::Flag::kCycleType_Mask)) >>
                    1;
                if (rawCycle == 1)
                {
                    track.cycle = PreviewAnimationCycle::Reverse;
                }
                else if (rawCycle == 2)
                {
                    track.cycle = PreviewAnimationCycle::Clamp;
                }
                else
                {
                    track.cycle = PreviewAnimationCycle::Loop;
                }
                track.frequency = std::isfinite(ctrl->frequency) && ctrl->frequency > 0.0f
                    ? ctrl->frequency : 1.0f;
                track.phase = std::isfinite(ctrl->phase) ? ctrl->phase : 0.0f;
                track.startTime = std::isfinite(ctrl->loKeyTime) ? ctrl->loKeyTime : 0.0f;
                track.stopTime = std::isfinite(ctrl->hiKeyTime) ? ctrl->hiKeyTime : 0.0f;

                track.keys.reserve(floatData->numKeys);
                const auto* keyBytes = reinterpret_cast<const std::uint8_t*>(floatData->keys);
                const std::size_t stride = floatData->keySize;
                for (std::uint32_t k = 0; k < floatData->numKeys; ++k)
                {
                    float keyTime = 0.0f;
                    float keyVal = 0.0f;
                    std::memcpy(&keyTime, keyBytes + k * stride, sizeof(float));
                    std::memcpy(&keyVal, keyBytes + k * stride + sizeof(float), sizeof(float));
                    if (std::isfinite(keyTime) && std::isfinite(keyVal))
                    {
                        track.keys.push_back({keyTime, keyVal});
                    }
                }
                if (track.keys.size() >= 2)
                {
                    a_material.floatTracks.push_back(std::move(track));
                }
            }
        }

        bool TryExtractMaterial(RE::BSTriShape* a_shape,
                                bool a_preferRuntimeTextures,
                                PreviewMaterial& a_material)
        {
            a_material = PreviewMaterial{};
            if (a_shape == nullptr)
            {
                return false;
            }

            const auto* rawShapeName = a_shape->name.c_str();
            const std::string_view shapeName =
                rawShapeName != nullptr ? std::string_view(rawShapeName) : std::string_view{};
            if (IsWorldHelperNodeName(shapeName))
            {
                return false;
            }

            const auto& geometryData = a_shape->GetGeometryRuntimeData();
            auto* rawShader = geometryData.shaderProperty.get();
            if (rawShader == nullptr)
            {
                // O motor do Skyrim nao renderiza geometria sem shader (ex.: SalmonRoe01:10).
                return false;
            }

            if (auto* effectShader = CastEffectShaderProperty(rawShader); effectShader != nullptr)
            {
                if (effectShader->flags.any(
                        RE::BSShaderProperty::EShaderPropertyFlag::kRefraction,
                        RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction))
                {
                    return false;
                }
                auto* baseMaterial = effectShader->GetBaseMaterial();
                if (baseMaterial == nullptr ||
                    baseMaterial->GetType() != RE::BSShaderMaterial::Type::kEffect)
                {
                    return false;
                }
                auto* effectMaterial = static_cast<RE::BSEffectShaderMaterial*>(baseMaterial);

                if (const auto* src = effectMaterial->sourceTexturePath.c_str();
                    src != nullptr && src[0] != '\0')
                {
                    a_material.diffuseTexturePath = src;
                }
                if (effectMaterial->sourceTexture != nullptr)
                {
                    RecoverRuntimeTexturePath(
                        a_material.diffuseTexturePath,
                        effectMaterial->sourceTexture->name.c_str(),
                        a_preferRuntimeTextures);
                }
                if (const auto* grey = effectMaterial->greyscaleTexturePath.c_str();
                    grey != nullptr && grey[0] != '\0')
                {
                    a_material.glowTexturePath = grey;
                }
                if (effectMaterial->greyscaleTexture != nullptr)
                {
                    RecoverRuntimeTexturePath(
                        a_material.glowTexturePath,
                        effectMaterial->greyscaleTexture->name.c_str(),
                        a_preferRuntimeTextures);
                }

                // Dummies de luz ENB / Particle Patch (ex.: ppLight com pp_black.dds)
                // sao invisiveis no jogo e nao devem distorcer o enquadramento nem virar malha branca.
                if (IsEnbLightDummyTexture(a_material.diffuseTexturePath))
                {
                    return false;
                }

                a_material.feature = PreviewMaterialFeature::Effect;
                a_material.effectShader = true;
                a_material.twoSided = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
                a_material.premultipliedAlpha = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kPremultAlpha);
                a_material.vertexColors = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
                a_material.vertexAlpha = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha);
                a_material.effectLighting = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting);
                a_material.grayscaleToPaletteColor = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor);
                a_material.grayscaleToPaletteAlpha = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha);
                a_material.falloffEnabled = effectShader->flags.all(
                    RE::BSShaderProperty::EShaderPropertyFlag::kFalloff);
                a_material.glowMapEnabled = !a_material.glowTexturePath.empty();

                const auto& bc = effectMaterial->baseColor;
                a_material.tintColor = {
                    std::isfinite(bc.red) ? std::clamp(bc.red, 0.0f, 16.0f) : 1.0f,
                    std::isfinite(bc.green) ? std::clamp(bc.green, 0.0f, 16.0f) : 1.0f,
                    std::isfinite(bc.blue) ? std::clamp(bc.blue, 0.0f, 16.0f) : 1.0f,
                };
                a_material.opacity = std::isfinite(bc.alpha) ? std::clamp(bc.alpha, 0.0f, 1.0f) : 1.0f;
                const float scale = effectMaterial->baseColorScale;
                a_material.emissiveMultiple =
                    std::isfinite(scale) ? std::clamp(scale, 0.0f, 16.0f) : 1.0f;
                a_material.falloffStartAngle = std::isfinite(effectMaterial->falloffStartAngle)
                    ? effectMaterial->falloffStartAngle : 1.0f;
                a_material.falloffStopAngle = std::isfinite(effectMaterial->falloffStopAngle)
                    ? effectMaterial->falloffStopAngle : 0.0f;
                a_material.falloffStartOpacity = std::isfinite(effectMaterial->falloffStartOpacity)
                    ? std::clamp(effectMaterial->falloffStartOpacity, 0.0f, 1.0f) : 1.0f;
                a_material.falloffStopOpacity = std::isfinite(effectMaterial->falloffStopOpacity)
                    ? std::clamp(effectMaterial->falloffStopOpacity, 0.0f, 1.0f) : 1.0f;
                a_material.textureOffset = {
                    effectMaterial->texCoordOffset[0].x,
                    effectMaterial->texCoordOffset[0].y,
                };
                a_material.textureScale = {
                    effectMaterial->texCoordScale[0].x,
                    effectMaterial->texCoordScale[0].y,
                };

                ReadAlphaProperty(geometryData.alphaProperty.get(), a_material);
                if (geometryData.alphaProperty == nullptr && a_material.opacity < 0.999f)
                {
                    a_material.alphaBlend = true;
                }
                ExtractShaderFloatControllers(effectShader, true, a_material);
                return true;
            }

            auto* shader = a_shape->lightingShaderProp_cast();
            if (shader == nullptr)
            {
                return false;
            }
            if (shader->flags.any(
                    RE::BSShaderProperty::EShaderPropertyFlag::kRefraction,
                    RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction))
            {
                // Planos de distorcao de calor/refracao (ex.: HeatRefraction:0 na tocha)
                // nao possuem framebuffer de refracao na previa e nao devem ser desenhados.
                return false;
            }

            auto& material = a_material;
            material.twoSided = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
            material.premultipliedAlpha = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kPremultAlpha);
            material.specularEnabled = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kSpecular);
            material.envmapLightFade = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kEnvmapLightFade);
            material.vertexColors = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
            material.vertexAlpha = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha);
            material.ownEmit = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
            material.glowMapEnabled = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kGlowMap);
            const auto usesModelSpaceNormals = shader->flags.all(
                RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
            material.opacity = std::clamp(shader->QMaterialAlpha(), 0.0f, 1.0f);

            if (shader->emissiveColor != nullptr)
            {
                const float er = shader->emissiveColor->red;
                const float eg = shader->emissiveColor->green;
                const float eb = shader->emissiveColor->blue;
                if (std::isfinite(er) && std::isfinite(eg) && std::isfinite(eb))
                {
                    material.emissiveColor = {
                        std::clamp(er, 0.0f, 16.0f),
                        std::clamp(eg, 0.0f, 16.0f),
                        std::clamp(eb, 0.0f, 16.0f),
                    };
                }
            }
            material.emissiveMultiple = std::isfinite(shader->emissiveMult)
                ? std::clamp(shader->emissiveMult, 0.0f, 16.0f)
                : 0.0f;

            ReadAlphaProperty(geometryData.alphaProperty.get(), material);
            ExtractShaderFloatControllers(shader, false, material);

            auto* baseMaterial = shader->GetBaseMaterial();
            if (baseMaterial == nullptr ||
                baseMaterial->GetType() != RE::BSShaderMaterial::Type::kLighting)
            {
                return true;
            }
            auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(baseMaterial);
            if (baseMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap)
            {
                material.glowMapEnabled = true;
            }
            material.specularColor = {
                lightingMaterial->specularColor.red,
                lightingMaterial->specularColor.green,
                lightingMaterial->specularColor.blue,
            };
            material.specularPower = std::clamp(lightingMaterial->specularPower, 1.0f, 512.0f);
            material.specularStrength = std::clamp(
                lightingMaterial->specularColorScale, 0.0f, 16.0f);
            material.textureOffset = {
                lightingMaterial->texCoordOffset[0].x,
                lightingMaterial->texCoordOffset[0].y,
            };
            material.textureScale = {
                lightingMaterial->texCoordScale[0].x,
                lightingMaterial->texCoordScale[0].y,
            };
            const auto copyTint = [&](const RE::NiColor& a_color,
                                      MaterialTintKind a_kind) {
                const Float3 tint{a_color.red, a_color.green, a_color.blue};
                if (IsValidMaterialTint(tint))
                {
                    material.tintColor = tint;
                    material.tintKind = a_kind;
                }
            };
            switch (baseMaterial->GetFeature())
            {
            case RE::BSShaderMaterial::Feature::kFaceGen:
            {
                material.feature = PreviewMaterialFeature::FaceGen;
                auto* faceGen = static_cast<RE::BSLightingShaderMaterialFacegen*>(
                    lightingMaterial);
                const auto copySourceTexturePath = [](RE::NiSourceTexture* a_texture,
                                                      std::string& a_destination) {
                    if (a_texture == nullptr)
                    {
                        return;
                    }
                    const auto* name = a_texture->name.c_str();
                    if (name != nullptr && name[0] != '\0')
                    {
                        a_destination = name;
                    }
                };
                copySourceTexturePath(faceGen->tintTexture.get(),
                                      material.faceTintTexturePath);
                copySourceTexturePath(faceGen->detailTexture.get(),
                                      material.faceDetailTexturePath);
                copySourceTexturePath(faceGen->subsurfaceTexture.get(),
                                      material.faceSubsurfaceTexturePath);
                break;
            }
            case RE::BSShaderMaterial::Feature::kFaceGenRGBTint:
                material.feature = PreviewMaterialFeature::FaceGenRgbTint;
                copyTint(
                    static_cast<RE::BSLightingShaderMaterialFacegenTint*>(
                        lightingMaterial)->tintColor,
                    MaterialTintKind::Body);
                break;
            case RE::BSShaderMaterial::Feature::kHairTint:
                material.feature = PreviewMaterialFeature::HairTint;
                copyTint(
                    static_cast<RE::BSLightingShaderMaterialHairTint*>(
                        lightingMaterial)->tintColor,
                    MaterialTintKind::Hair);
                break;
            default:
                break;
            }
            if (const auto textureSet = lightingMaterial->GetTextureSet(); textureSet != nullptr)
            {
                const auto readTexturePath = [&](std::uint32_t a_slot, std::string& a_destination) {
                    if (const auto* path = textureSet->GetTexturePath(
                            static_cast<RE::BSTextureSet::Texture>(a_slot));
                        path != nullptr)
                    {
                        a_destination = path;
                    }
                };
                readTexturePath(RE::BSTextureSet::Textures::kDiffuse,
                                material.diffuseTexturePath);
                readTexturePath(RE::BSTextureSet::Textures::kNormal,
                                material.normalTexturePath);
                // Slot 2 = glow map (_g.dds) no BSShaderTextureSet do Skyrim.
                constexpr std::uint32_t kGlowMapSlot = 2;
                if (material.glowMapEnabled)
                {
                    readTexturePath(kGlowMapSlot, material.glowTexturePath);
                }
                // Aetherius: slots reais do BSShaderTextureSet (4 = cubemap, 5 = máscara
                // de reflexo). O enum do CommonLib diz kEnvironment = 5 e kEnvironmentMask = 2,
                // o que carregava o _m.dds como cubemap e deixava o metal sem reflexo.
                constexpr std::uint32_t kEnvironmentSlot = 4;
                constexpr std::uint32_t kEnvironmentMaskSlot = 5;
                readTexturePath(kEnvironmentSlot, material.environmentTexturePath);
                readTexturePath(kEnvironmentMaskSlot, material.environmentMaskTexturePath);
                readTexturePath(RE::BSTextureSet::Textures::kSpecular,
                                material.specularTexturePath);
                if (material.feature == PreviewMaterialFeature::FaceGen)
                {
                    readTexturePath(RE::BSTextureSet::Textures::kMultilayer,
                                    material.faceTintTexturePath);
                    readTexturePath(RE::BSTextureSet::Textures::kDetailMap,
                                    material.faceDetailTexturePath);
                    readTexturePath(RE::BSTextureSet::Textures::kSubsurfaceTint,
                                    material.faceSubsurfaceTexturePath);
                }
                material.tangentSpaceNormal =
                    !usesModelSpaceNormals && !material.normalTexturePath.empty();
                material.modelSpaceNormal =
                    usesModelSpaceNormals && !material.normalTexturePath.empty();

                if (baseMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap)
                {
                    auto* envMaterial = static_cast<RE::BSLightingShaderMaterialEnvmap*>(
                        lightingMaterial);
                    // Aetherius: escala inválida (NaN/fora da faixa) deixava o metal preto
                    // (NaN) ou estourado em branco. O padrão do jogo é 1.
                    const float rawScale = envMaterial->envMapScale;
                    material.environmentScale =
                        std::isfinite(rawScale) && rawScale >= 0.0f && rawScale <= 16.0f ? rawScale : 1.0f;
                    spdlog::info("SkyrimNifExtractor: envmap '{}' raw scale {} -> {}",
                                 material.environmentTexturePath, rawScale, material.environmentScale);
                    material.environmentEnabled =
                        !material.environmentTexturePath.empty() &&
                        shader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kEnvMap);
                }
            }
            if (a_preferRuntimeTextures || material.diffuseTexturePath.empty() ||
                material.normalTexturePath.empty() ||
                (material.glowMapEnabled && material.glowTexturePath.empty()) ||
                (baseMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap &&
                 material.environmentTexturePath.empty()))
            {
                // Cached models can have live NiSourceTextures after their
                // texture set has been cleared. Retain valid record/override
                // paths, but recover missing paths from the loaded material.
                const auto copySourceTexturePath = [a_preferRuntimeTextures](RE::NiSourceTexture* a_texture,
                                                      std::string& a_destination) {
                    if (a_texture == nullptr)
                    {
                        return;
                    }
                    const auto* name = a_texture->name.c_str();
                    RecoverRuntimeTexturePath(a_destination, name, a_preferRuntimeTextures);
                };
                copySourceTexturePath(lightingMaterial->diffuseTexture.get(),
                                      material.diffuseTexturePath);
                copySourceTexturePath(lightingMaterial->normalTexture.get(),
                                      material.normalTexturePath);
                copySourceTexturePath(lightingMaterial->specularBackLightingTexture.get(),
                                      material.specularTexturePath);
                if (baseMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap)
                {
                    auto* glowMaterial = static_cast<RE::BSLightingShaderMaterialGlowmap*>(
                        lightingMaterial);
                    copySourceTexturePath(glowMaterial->glowTexture.get(),
                                          material.glowTexturePath);
                }
                if (baseMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap)
                {
                    auto* envMaterial = static_cast<RE::BSLightingShaderMaterialEnvmap*>(
                        lightingMaterial);
                    copySourceTexturePath(envMaterial->envTexture.get(),
                                          material.environmentTexturePath);
                    copySourceTexturePath(envMaterial->envMaskTexture.get(),
                                          material.environmentMaskTexturePath);
                    const float rawScale = envMaterial->envMapScale;
                    material.environmentScale =
                        std::isfinite(rawScale) && rawScale >= 0.0f && rawScale <= 16.0f ? rawScale : 1.0f;
                    material.environmentEnabled =
                        !material.environmentTexturePath.empty() &&
                        shader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kEnvMap);
                }
                material.tangentSpaceNormal =
                    !usesModelSpaceNormals && !material.normalTexturePath.empty();
                material.modelSpaceNormal =
                    usesModelSpaceNormals && !material.normalTexturePath.empty();
            }
            return true;
        }

        ShapeResult DecodeRendererGeometry(RE::BSGraphics::TriShape* a_rendererData,
                                           std::size_t a_vertexCount,
                                           std::size_t a_indexCount,
                                           bool a_requireSkinning,
                                           ID3D11Device* a_device,
                                           ID3D11DeviceContext* a_context,
                                           PreviewMesh& a_output,
                                           PackedPositionStream a_positionStream = {})
        {
            if (a_rendererData == nullptr || a_vertexCount == 0 || a_indexCount == 0)
            {
                return ShapeResult::Unsupported;
            }
            auto vertexDesc = a_rendererData->vertexDesc;
            if ((!vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) &&
                 a_positionStream.bytes.empty()) ||
                (a_requireSkinning &&
                 !vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)))
            {
                return ShapeResult::Unsupported;
            }
            const auto stride = static_cast<std::size_t>(vertexDesc.GetSize());
            if (stride == 0 || a_vertexCount > MAX_PREVIEW_VERTICES ||
                a_indexCount > MAX_PREVIEW_INDICES)
            {
                return ShapeResult::Malformed;
            }

            const auto vertexBytesRequired = a_vertexCount * stride;
            const auto indexBytesRequired = a_indexCount * sizeof(std::uint16_t);
            std::vector<std::uint8_t> ownedVertexBytes;
            std::vector<std::uint8_t> ownedIndexBytes;
            std::span<const std::uint8_t> vertexBytes;
            std::span<const std::uint16_t> indices;

            if (a_rendererData->rawVertexData != nullptr)
            {
                vertexBytes = {a_rendererData->rawVertexData, vertexBytesRequired};
            }
            else
            {
                auto* vertexBuffer =
                    reinterpret_cast<ID3D11Buffer*>(a_rendererData->vertexBuffer);
                if (!CopyGpuBuffer(a_device, a_context, vertexBuffer, vertexBytesRequired, ownedVertexBytes))
                {
                    return ShapeResult::ReadbackFailed;
                }
                vertexBytes = ownedVertexBytes;
            }

            if (a_rendererData->rawIndexData != nullptr)
            {
                indices = {a_rendererData->rawIndexData, a_indexCount};
            }
            else
            {
                auto* indexBuffer =
                    reinterpret_cast<ID3D11Buffer*>(a_rendererData->indexBuffer);
                if (!CopyGpuBuffer(a_device, a_context, indexBuffer, indexBytesRequired, ownedIndexBytes))
                {
                    return ShapeResult::ReadbackFailed;
                }
                indices = {
                    reinterpret_cast<const std::uint16_t*>(ownedIndexBytes.data()),
                    a_indexCount,
                };
            }

            PackedGeometryLayout layout{};
            layout.stride = stride;
            if (vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
            {
                layout.positionOffset =
                    vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
            }
            layout.hasPackedNormal = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
            if (layout.hasPackedNormal)
            {
                layout.normalOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
            }
            layout.hasPackedTangent = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT);
            if (layout.hasPackedTangent)
            {
                layout.tangentOffset =
                    vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL);
            }
            layout.hasTextureCoordinate = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV);
            if (layout.hasTextureCoordinate)
            {
                layout.textureCoordinateOffset =
                    vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
            }
            layout.hasVertexColor = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
            if (layout.hasVertexColor)
            {
                layout.colorOffset =
                    vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
            }
            layout.hasSkinning = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
            if (layout.hasSkinning)
            {
                layout.skinningOffset =
                    vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
            }

            auto decoded = DecodePackedGeometry(
                vertexBytes,
                a_vertexCount,
                indices,
                layout,
                a_positionStream);
            if (decoded.error != DecodeError::None)
            {
                return ShapeResult::Malformed;
            }
            a_output = std::move(decoded.mesh);
            return ShapeResult::Accepted;
        }

        void ApplyMaterial(PreviewMesh& a_mesh, const PreviewMaterial& a_material)
        {
            // Aetherius: textureScale e textureOffset agora sao aplicados via gUvTransform no
            // pixel shader para permitir animacoes de UV em tempo real (ex.: liquido de pocoes).
            if (!a_mesh.draws.empty())
            {
                a_mesh.draws.front().material = a_material;
            }
        }

        ShapeResult ExtractRigidShape(RE::BSTriShape* a_shape,
                                      const AffineTransform& a_transform,
                                      ID3D11Device* a_device,
                                      ID3D11DeviceContext* a_context,
                                      const ModelTextureOverride* a_textureOverride,
                                      const ActorMaterialTintOverride* a_materialTint,
                                      bool a_preferRuntimeTextures,
                                      std::size_t& a_matchedMaterialTintOverrides,
                                      PreviewMesh& a_combined)
        {
            PreviewMaterial material{};
            if (!TryExtractMaterial(a_shape, a_preferRuntimeTextures, material))
            {
                return ShapeResult::Unsupported;
            }
            const auto& geometryData = a_shape->GetGeometryRuntimeData();
            const auto& shapeData = a_shape->GetTrishapeRuntimeData();
            PreviewMesh decoded{};
            const auto decodeResult = DecodeRendererGeometry(
                geometryData.rendererData,
                static_cast<std::size_t>(shapeData.vertexCount),
                static_cast<std::size_t>(shapeData.triangleCount) * 3,
                false,
                a_device,
                a_context,
                decoded);
            if (decodeResult != ShapeResult::Accepted)
            {
                return decodeResult;
            }

            decoded.bounds = {};
            if (a_textureOverride != nullptr)
            {
                ApplyTextureOverride(material, *a_textureOverride);
            }
            if (a_materialTint != nullptr &&
                ApplyActorMaterialTint(material, *a_materialTint))
            {
                ++a_matchedMaterialTintOverrides;
            }
            for (auto& vertex : decoded.vertices)
            {
                vertex.position = SkinningDetail::TransformPoint(a_transform, vertex.position);
                vertex.normal = Normalize(
                    SkinningDetail::TransformDirection(a_transform, vertex.normal));
                const auto rotatedTangent = SkinningDetail::TransformDirection(
                    a_transform,
                    {vertex.tangent.x, vertex.tangent.y, vertex.tangent.z});
                const auto normalizedTangent = Normalize(rotatedTangent);
                vertex.tangent.x = normalizedTangent.x;
                vertex.tangent.y = normalizedTangent.y;
                vertex.tangent.z = normalizedTangent.z;
                if (a_transform.scale < 0.0f)
                {
                    vertex.tangent.w = -vertex.tangent.w;
                }
                decoded.bounds.Include(vertex.position);
            }
            ApplyMaterial(decoded, material);
            return AppendMesh(a_combined, decoded) ? ShapeResult::Accepted : ShapeResult::Malformed;
        }

        ShapeResult BuildSkinBones(RE::NiSkinInstance* a_skinInstance,
                                   const ObjectTransformMap& a_transforms,
                                   std::vector<BindPoseBone>& a_bones,
                                   std::size_t& a_reconstructedExternalBones)
        {
            if (a_skinInstance == nullptr || a_skinInstance->skinData == nullptr)
            {
                return ShapeResult::Unsupported;
            }
            const auto* skinData = a_skinInstance->skinData.get();
            const auto boneCount = static_cast<std::size_t>(skinData->GetBoneCount());
            if (boneCount == 0)
            {
                return ShapeResult::Unsupported;
            }
            if (boneCount > MAX_SKIN_BONES || skinData->boneData == nullptr ||
                a_skinInstance->bones == nullptr)
            {
                return ShapeResult::Malformed;
            }

            a_bones.resize(boneCount);
            for (std::size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
            {
                const auto skinToBone =
                    ToAffineTransform(skinData->GetBoneDataSkinToBone(static_cast<std::uint32_t>(boneIndex)));
                a_bones[boneIndex].skinToBone = skinToBone;
                auto* boneObject = a_skinInstance->bones[boneIndex];
                const auto transform = a_transforms.find(boneObject);
                if (boneObject == nullptr || transform == a_transforms.end())
                {
                    if (!TryInvertAffine(skinToBone, a_bones[boneIndex].boneToModel))
                    {
                        return ShapeResult::Malformed;
                    }
                    ++a_reconstructedExternalBones;
                    continue;
                }
                a_bones[boneIndex].boneToModel = transform->second;
            }
            return ShapeResult::Accepted;
        }

        ShapeResult ExtractSkinnedShape(RE::BSTriShape* a_shape,
                                        const ObjectTransformMap& a_transforms,
                                        ID3D11Device* a_device,
                                        ID3D11DeviceContext* a_context,
                                        const ModelTextureOverride* a_textureOverride,
                                        const ActorMaterialTintOverride* a_materialTint,
                                        bool a_preferRuntimeTextures,
                                        std::size_t& a_matchedMaterialTintOverrides,
                                        std::size_t& a_reconstructedExternalBones,
                                        std::size_t& a_decodedDynamicShapes,
                                        PreviewMesh& a_combined)
        {
            const auto& geometryData = a_shape->GetGeometryRuntimeData();
            auto* skinInstance = geometryData.skinInstance.get();
            std::vector<BindPoseBone> skinBones;
            const auto boneResult = BuildSkinBones(
                skinInstance,
                a_transforms,
                skinBones,
                a_reconstructedExternalBones);
            if (boneResult != ShapeResult::Accepted)
            {
                return boneResult;
            }

            auto* skinPartition = skinInstance->skinPartition.get();
            if (skinPartition == nullptr && skinInstance->skinData != nullptr)
            {
                skinPartition = skinInstance->skinData->skinPartition.get();
            }
            if (skinPartition == nullptr || skinPartition->numPartitions == 0)
            {
                return ShapeResult::Unsupported;
            }
            const auto partitionCount = std::min<std::size_t>(
                skinPartition->numPartitions, skinPartition->partitions.size());
            if (partitionCount == 0 || partitionCount > MAX_SKIN_PARTITIONS ||
                partitionCount != skinPartition->numPartitions)
            {
                return ShapeResult::Malformed;
            }
            const auto sharedVertexCount = static_cast<std::size_t>(
                skinPartition->vertexCount);
            if (sharedVertexCount == 0 || sharedVertexCount > MAX_PREVIEW_VERTICES)
            {
                return ShapeResult::Malformed;
            }

            std::vector<std::uint8_t> dynamicPositionBytes;
            PackedPositionStream positionStream{};
            const bool isDynamic = a_shape->AsDynamicTriShape() != nullptr;
            if (isDynamic)
            {
                constexpr std::size_t DYNAMIC_POSITION_STRIDE = sizeof(float) * 4;
                if (sharedVertexCount >
                    std::numeric_limits<std::size_t>::max() / DYNAMIC_POSITION_STRIDE)
                {
                    return ShapeResult::Malformed;
                }
                const auto bytesRequired = sharedVertexCount * DYNAMIC_POSITION_STRIDE;
                auto& dynamicData =
                    a_shape->AsDynamicTriShape()->GetDynamicTrishapeRuntimeData();
                {
                    RE::BSSpinLockGuard locker(dynamicData.lock);
                    if (dynamicData.dynamicData == nullptr ||
                        dynamicData.dataSize < bytesRequired)
                    {
                        return ShapeResult::Malformed;
                    }
                    dynamicPositionBytes.resize(bytesRequired);
                    std::memcpy(
                        dynamicPositionBytes.data(),
                        dynamicData.dynamicData,
                        bytesRequired);
                }
                positionStream = {
                    .bytes = dynamicPositionBytes,
                    .stride = DYNAMIC_POSITION_STRIDE,
                };
            }

            PreviewMesh shapeMesh{};
            PreviewMaterial material{};
            if (!TryExtractMaterial(a_shape, a_preferRuntimeTextures, material))
            {
                return ShapeResult::Unsupported;
            }
            if (a_textureOverride != nullptr)
            {
                ApplyTextureOverride(material, *a_textureOverride);
            }
            if (a_materialTint != nullptr &&
                ApplyActorMaterialTint(material, *a_materialTint))
            {
                ++a_matchedMaterialTintOverrides;
            }
            for (std::size_t partitionIndex = 0; partitionIndex < partitionCount; ++partitionIndex)
            {
                const auto& partition = skinPartition->partitions[partitionIndex];
                const auto paletteCount = static_cast<std::size_t>(partition.numBones);
                if (paletteCount == 0 || paletteCount > MAX_PARTITION_BONES ||
                    partition.bones == nullptr)
                {
                    return ShapeResult::Malformed;
                }

                for (std::size_t paletteIndex = 0; paletteIndex < paletteCount; ++paletteIndex)
                {
                    const auto skinBoneIndex = static_cast<std::size_t>(
                        partition.bones[paletteIndex]);
                    if (skinBoneIndex >= skinBones.size())
                    {
                        return ShapeResult::Malformed;
                    }
                }
                if (partition.vertices == 0 ||
                    static_cast<std::size_t>(partition.vertices) > sharedVertexCount)
                {
                    return ShapeResult::Malformed;
                }

                PreviewMesh decoded{};
                const auto decodeResult = DecodeRendererGeometry(
                    partition.buffData,
                    sharedVertexCount,
                    static_cast<std::size_t>(partition.triangles) * 3,
                    true,
                    a_device,
                    a_context,
                    decoded,
                    positionStream);
                if (decodeResult != ShapeResult::Accepted)
                {
                    return decodeResult;
                }
                if (ApplyBindPoseSkinning(decoded, skinBones) != SkinningError::None)
                {
                    return ShapeResult::Malformed;
                }
                ApplyMaterial(decoded, material);
                if (!AppendMesh(shapeMesh, decoded))
                {
                    return ShapeResult::Malformed;
                }
            }

            if (!AppendMesh(a_combined, shapeMesh))
            {
                return ShapeResult::Malformed;
            }
            if (isDynamic)
            {
                ++a_decodedDynamicShapes;
            }
            return ShapeResult::Accepted;
        }

        ShapeResult ExtractShape(RE::BSTriShape* a_shape,
                                 const AffineTransform& a_transform,
                                 const ObjectTransformMap& a_transforms,
                                 ID3D11Device* a_device,
                                 ID3D11DeviceContext* a_context,
                                 const ModelTextureOverride* a_textureOverride,
                                 const ActorMaterialTintOverride* a_materialTint,
                                 bool a_preferRuntimeTextures,
                                 std::size_t& a_matchedMaterialTintOverrides,
                                 std::size_t& a_reconstructedExternalBones,
                                 std::size_t& a_decodedDynamicShapes,
                                 PreviewMesh& a_combined)
        {
            if (a_shape == nullptr)
            {
                return ShapeResult::Unsupported;
            }
            return a_shape->GetGeometryRuntimeData().skinInstance != nullptr
                       ? ExtractSkinnedShape(
                             a_shape,
                             a_transforms,
                             a_device,
                             a_context,
                             a_textureOverride,
                             a_materialTint,
                             a_preferRuntimeTextures,
                             a_matchedMaterialTintOverrides,
                             a_reconstructedExternalBones,
                             a_decodedDynamicShapes,
                             a_combined)
                       : ExtractRigidShape(
                             a_shape,
                             a_transform,
                             a_device,
                             a_context,
                             a_textureOverride,
                             a_materialTint,
                             a_preferRuntimeTextures,
                             a_matchedMaterialTintOverrides,
                             a_combined);
        }

        void Traverse(RE::NiAVObject* a_object,
                      const ObjectTransformMap& a_transforms,
                      ID3D11Device* a_device,
                       ID3D11DeviceContext* a_context,
                       std::span<const ModelTextureOverride> a_textureOverrides,
                       const ActorMaterialTintOverride* a_materialTint,
                       bool a_skipCulled,
                       bool a_preferRuntimeTextures,
                       ExtractionResult& a_result,
                      bool& a_sawMalformed,
                      bool& a_sawReadbackFailure,
                      std::uint32_t& a_shapeIndex,
                      std::unordered_set<const RE::NiAVObject*>& a_visited)
        {
            if (a_object == nullptr || !a_visited.insert(a_object).second)
            {
                return;
            }
            if (a_skipCulled && a_object->GetAppCulled())
            {
                if (a_object->AsTriShape() != nullptr)
                {
                    ++a_result.skippedShapes;
                }
                return;
            }
            if (const auto* rawObjectName = a_object->name.c_str();
                rawObjectName != nullptr && IsWorldHelperNodeName(rawObjectName))
            {
                if (a_object->AsTriShape() != nullptr)
                {
                    ++a_result.skippedShapes;
                }
                return;
            }

            const auto transform = a_transforms.find(a_object);
            if (transform == a_transforms.end())
            {
                a_sawMalformed = true;
                return;
            }
            if (auto* shape = a_object->AsTriShape(); shape != nullptr)
            {
                const auto currentShapeIndex = a_shapeIndex++;
                const auto* rawShapeName = shape->name.c_str();
                const std::string_view shapeName = rawShapeName != nullptr ?
                    std::string_view(rawShapeName) : std::string_view{};
                const auto* textureOverride = FindTextureOverride(
                    a_textureOverrides, shapeName, currentShapeIndex);
                if (textureOverride != nullptr)
                {
                    ++a_result.matchedTextureOverrides;
                }
                switch (ExtractShape(
                    shape,
                    transform->second,
                    a_transforms,
                    a_device,
                    a_context,
                    textureOverride,
                    a_materialTint,
                    a_preferRuntimeTextures,
                    a_result.matchedMaterialTintOverrides,
                    a_result.reconstructedExternalBones,
                    a_result.decodedDynamicShapes,
                    a_result.mesh))
                {
                case ShapeResult::Accepted:
                    ++a_result.acceptedShapes;
                    break;
                case ShapeResult::Unsupported:
                    ++a_result.skippedShapes;
                    break;
                case ShapeResult::Malformed:
                    ++a_result.skippedShapes;
                    a_sawMalformed = true;
                    if (a_result.firstFailedShapeName.empty())
                    {
                        const auto* name = shape->name.c_str();
                        a_result.firstFailedShapeName =
                            name != nullptr && name[0] != '\0' ? name : "<unnamed>";
                    }
                    break;
                case ShapeResult::ReadbackFailed:
                    ++a_result.skippedShapes;
                    a_sawReadbackFailure = true;
                    if (a_result.firstFailedShapeName.empty())
                    {
                        const auto* name = shape->name.c_str();
                        a_result.firstFailedShapeName =
                            name != nullptr && name[0] != '\0' ? name : "<unnamed>";
                    }
                    break;
                }
            }

            if (auto* node = a_object->AsNode(); node != nullptr)
            {
                for (const auto& child : node->GetChildren())
                {
                    if (child != nullptr)
                    {
                        Traverse(
                            child.get(),
                            a_transforms,
                            a_device,
                            a_context,
                            a_textureOverrides,
                            a_materialTint,
                            a_skipCulled,
                            a_preferRuntimeTextures,
                            a_result,
                            a_sawMalformed,
                            a_sawReadbackFailure,
                            a_shapeIndex,
                            a_visited);
                    }
                }
            }
        }
    }

    ExtractionResult ExtractModelMesh(RE::NiNode* a_root,
                                      ID3D11Device* a_device,
                                      ID3D11DeviceContext* a_context,
                                      std::span<const ModelTextureOverride> a_textureOverrides,
                                      const ActorMaterialTintOverride* a_materialTint)
    {
        ExtractionResult result{};
        if (a_root == nullptr || a_device == nullptr || a_context == nullptr)
        {
            result.status = ExtractionStatus::MalformedGeometry;
            return result;
        }

        ObjectTransformMap transforms;
        transforms.reserve(1'024);
        if (!CollectObjectTransforms(a_root, AffineTransform{}, transforms, 0))
        {
            result.status = ExtractionStatus::MalformedGeometry;
            return result;
        }

        bool sawMalformed = false;
        bool sawReadbackFailure = false;
        std::uint32_t shapeIndex = 0;
        std::unordered_set<const RE::NiAVObject*> visited;
        visited.reserve(transforms.size());
        // Aetherius: peças ocultas no NIF (flag 0x1) ficam fora da prévia, como no jogo — o
        // sangue das armas (BloodEdge01Add, aditivo) e a bainha esquerda deixavam lâminas e
        // talheres brancos. Se o modelo inteiro estiver oculto, desenha sem pular.
        for (const bool skipCulled : {true, false})
        {
            if (!skipCulled && (!result.mesh.Empty() || result.skippedShapes == 0))
            {
                break;
            }
            result = ExtractionResult{};
            sawMalformed = false;
            sawReadbackFailure = false;
            shapeIndex = 0;
            visited.clear();
            Traverse(
                a_root,
                transforms,
                a_device,
                a_context,
                a_textureOverrides,
                a_materialTint,
                skipCulled,
                false,
                result,
                sawMalformed,
                sawReadbackFailure,
                shapeIndex,
                visited);
        }
        if (!result.mesh.Empty())
        {
            result.status = ExtractionStatus::Success;
        }
        else if (sawReadbackFailure)
        {
            result.status = ExtractionStatus::GpuReadbackFailed;
        }
        else if (sawMalformed)
        {
            result.status = ExtractionStatus::MalformedGeometry;
        }
        else
        {
            result.status = ExtractionStatus::NoSupportedGeometry;
        }
        return result;
    }

    ExtractionResult ExtractLiveActorMesh(
        RE::NiNode* a_actorRoot,
        std::span<RE::NiAVObject* const> a_selectedRoots,
        ID3D11Device* a_device,
        ID3D11DeviceContext* a_context)
    {
        ExtractionResult result{};
        if (a_actorRoot == nullptr || a_selectedRoots.empty() ||
            a_device == nullptr || a_context == nullptr)
        {
            result.status = ExtractionStatus::MalformedGeometry;
            return result;
        }

        ObjectTransformMap transforms;
        transforms.reserve(4'096);
        const auto actorWorld = ToAffineTransform(a_actorRoot->world);
        if (!CollectLiveObjectTransforms(a_actorRoot, actorWorld, transforms, 0))
        {
            result.status = ExtractionStatus::MalformedGeometry;
            return result;
        }

        bool sawMalformed = false;
        bool sawReadbackFailure = false;
        std::uint32_t shapeIndex = 0;
        std::unordered_set<const RE::NiAVObject*> visited;
        visited.reserve(transforms.size());
        for (auto* root : a_selectedRoots)
        {
            if (root == nullptr || !transforms.contains(root))
            {
                sawMalformed = true;
                continue;
            }
            Traverse(
                root,
                transforms,
                a_device,
                a_context,
                {},
                nullptr,
                true,
                true,
                result,
                sawMalformed,
                sawReadbackFailure,
                shapeIndex,
                visited);
        }

        if (!result.mesh.Empty())
        {
            result.status = ExtractionStatus::Success;
        }
        else if (sawReadbackFailure)
        {
            result.status = ExtractionStatus::GpuReadbackFailed;
        }
        else if (sawMalformed)
        {
            result.status = ExtractionStatus::MalformedGeometry;
        }
        else
        {
            result.status = ExtractionStatus::NoSupportedGeometry;
        }
        return result;
    }
}
