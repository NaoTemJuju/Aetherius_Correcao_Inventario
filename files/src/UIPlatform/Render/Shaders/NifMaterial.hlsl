#define MATERIAL_NORMAL_MAP 0x01u
#define MATERIAL_SPECULAR 0x02u
#define MATERIAL_SPECULAR_MAP 0x04u
#define MATERIAL_ENVIRONMENT_MAP 0x08u
#define MATERIAL_ALPHA_TEST 0x10u
#define MATERIAL_PREMULTIPLIED_ALPHA 0x20u
#define MATERIAL_MODEL_SPACE_NORMAL 0x40u
#define MATERIAL_FACEGEN 0x80u
#define MATERIAL_FACEGEN_RGB_TINT 0x100u
#define MATERIAL_TEXTURE_ALPHA 0x200u
#define MATERIAL_VERTEX_COLORS 0x400u
#define MATERIAL_VERTEX_ALPHA 0x800u
#define MATERIAL_GLOW_MAP 0x1000u
#define MATERIAL_OWN_EMIT 0x2000u
#define MATERIAL_ENVIRONMENT_MASK_MAP 0x4000u
#define MATERIAL_EFFECT_SHADER 0x8000u
#define MATERIAL_EFFECT_LIGHTING 0x10000u
#define MATERIAL_GRAYSCALE_COLOR 0x20000u
#define MATERIAL_GRAYSCALE_ALPHA 0x40000u
#define MATERIAL_FALLOFF 0x80000u
#define MATERIAL_ADDITIVE_BLEND 0x100000u
#define MATERIAL_ENVMAP_LIGHT_FADE 0x200000u

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 gWorldViewProjection;
    float4 gCameraPosition;
    float4 gAmbientTop;
    float4 gAmbientBottom;
    float4 gLightDirection[3];
    float4 gLightColor[3];
    // x: linear exposure multiplier
    float4 gPostProcessParams;
};

cbuffer MaterialConstants : register(b1)
{
    float4 gSpecularColorPower;
    // x: opacity, y: specular strength, z: environment scale, w: alpha threshold
    float4 gMaterialParams;
    float4 gTintColor;
    // rgb: emissive color, w: emissive / effect scale multiplier
    float4 gEmissiveColorScale;
    // x: startAngle, y: stopAngle, z: startOpacity, w: stopOpacity
    float4 gFalloffParams;
    // xy: UV scale, zw: UV offset (inclui animacao de shader em tempo real)
    float4 gUvTransform;
    uint gMaterialFlags;
    float3 gMaterialPadding;
};

Texture2D<float4> gDiffuseTexture : register(t0);
Texture2D<float4> gNormalTexture : register(t1);
Texture2D<float4> gSpecularTexture : register(t2);
TextureCube<float4> gEnvironmentTexture : register(t3);
Texture2D<float4> gEnvironmentMaskTexture : register(t4);
Texture2D<float4> gFaceTintTexture : register(t5);
Texture2D<float4> gFaceDetailTexture : register(t6);
Texture2D<float4> gGlowTexture : register(t7);
SamplerState gWrapSampler : register(s0);
SamplerState gClampSampler : register(s1);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 textureCoordinate : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 textureCoordinate : TEXCOORD3;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), gWorldViewProjection);
    output.worldPosition = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.textureCoordinate = input.textureCoordinate;
    output.color = input.color;
    return output;
}

float3 ToneMapACES(float3 linearColor)
{
    const float3 numerator = linearColor * (2.51f * linearColor + 0.03f);
    const float3 denominator = linearColor * (2.43f * linearColor + 0.59f) + 0.14f;
    return saturate(numerator / denominator);
}

float3 LinearToSrgb(float3 linearColor)
{
    const float3 lower = linearColor * 12.92f;
    const float3 upper = 1.055f * pow(max(linearColor, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(upper, lower, step(linearColor, 0.0031308f));
}

float3 SrgbToLinear(float3 srgbColor)
{
    const float3 clamped = max(srgbColor, 0.0f);
    const float3 lower = clamped / 12.92f;
    const float3 upper = pow((clamped + 0.055f) / 1.055f, 2.4f);
    return lerp(upper, lower, step(clamped, 0.04045f));
}

// Aetherius: usa Reinhard em baixas luminancias para nao esmagar metais escuros
// (Adaga de Ferro, Elmo Daedrico) e madeira escura no toe do ACES, transicionando
// suavemente para ToneMapACES em meios-tons e realces.
float3 EncodeLinearDisplay(float3 linearColor)
{
    const float3 exposed = max(linearColor, 0.0f) * gPostProcessParams.x;
    const float3 aces = ToneMapACES(exposed);
    const float3 reinhard = exposed / (1.0f + exposed);
    const float3 mapped = lerp(reinhard, aces, saturate(exposed * 2.5f));
    return LinearToSrgb(mapped);
}

float3 ApplyFaceGenOverlay(float3 diffuseColor, float3 tintColor, float3 detailColor)
{
    const float3 diffuseSquared = diffuseColor * diffuseColor;
    return (diffuseSquared + 2.0f * tintColor * diffuseColor -
            2.0f * tintColor * diffuseSquared) * detailColor;
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    const float2 uv = input.textureCoordinate * gUvTransform.xy + gUvTransform.zw;
    const float4 albedo = gDiffuseTexture.Sample(gWrapSampler, uv);
    const float3 viewDirection = normalize(gCameraPosition.xyz - input.worldPosition);

    // BSEffectShaderProperty path (asas translucidas, halos, chamas, espirais de brilho).
    if ((gMaterialFlags & MATERIAL_EFFECT_SHADER) != 0u)
    {
        const float3 vertexColor =
            (gMaterialFlags & MATERIAL_VERTEX_COLORS) != 0u ? SrgbToLinear(input.color.rgb) : 1.0f;
        const float vertexAlpha =
            (gMaterialFlags & MATERIAL_VERTEX_ALPHA) != 0u ? input.color.a : 1.0f;
        const float3 baseColor = gTintColor.rgb * vertexColor;

        float3 effectColor = albedo.rgb * baseColor;
        float effectAlpha = albedo.a * gMaterialParams.x * vertexAlpha;

        if ((gMaterialFlags & MATERIAL_GLOW_MAP) != 0u)
        {
            if ((gMaterialFlags & MATERIAL_GRAYSCALE_COLOR) != 0u)
            {
                const float paletteU = saturate(albedo.r);
                const float3 paletteRgb = gGlowTexture.Sample(gClampSampler, float2(paletteU, 0.5f)).rgb;
                effectColor = paletteRgb * baseColor;
            }
            if ((gMaterialFlags & MATERIAL_GRAYSCALE_ALPHA) != 0u)
            {
                const float paletteU = saturate(albedo.a);
                const float paletteAlpha = gGlowTexture.Sample(gClampSampler, float2(paletteU, 0.5f)).a;
                effectAlpha = paletteAlpha * gMaterialParams.x * vertexAlpha;
            }
        }

        if ((gMaterialFlags & MATERIAL_FALLOFF) != 0u)
        {
            float3 effectNormal = normalize(input.normal);
            if (!isFrontFace)
            {
                effectNormal = -effectNormal;
            }
            const float ndotv = abs(dot(effectNormal, viewDirection));
            const float denom = gFalloffParams.x - gFalloffParams.y;
            const float t = abs(denom) > 1.0e-4f ? saturate((ndotv - gFalloffParams.y) / denom) : 1.0f;
            effectAlpha *= saturate(lerp(gFalloffParams.w, gFalloffParams.z, t));
        }

        if ((gMaterialFlags & MATERIAL_ALPHA_TEST) != 0u && effectAlpha < gMaterialParams.w)
        {
            discard;
        }

        float3 litEffect = effectColor * max(gEmissiveColorScale.w, 0.0f);
        if ((gMaterialFlags & MATERIAL_EFFECT_LIGHTING) != 0u)
        {
            float3 effectNormal = normalize(input.normal);
            if (!isFrontFace)
            {
                effectNormal = -effectNormal;
            }
            const float hemi = saturate(effectNormal.z * 0.5f + 0.5f);
            float3 effLight = lerp(gAmbientBottom.rgb, gAmbientTop.rgb, hemi);
            [unroll]
            for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
            {
                const float3 lightDirection = normalize(gLightDirection[lightIndex].xyz);
                effLight += gLightColor[lightIndex].rgb * saturate(dot(effectNormal, lightDirection));
            }
            litEffect *= effLight;
        }

        const float3 displayColor = EncodeLinearDisplay(litEffect);
        const float clampedAlpha = saturate(effectAlpha);
        if ((gMaterialFlags & MATERIAL_ADDITIVE_BLEND) != 0u)
        {
            return float4(displayColor * clampedAlpha, 0.0f);
        }
        if ((gMaterialFlags & MATERIAL_PREMULTIPLIED_ALPHA) != 0u)
        {
            return float4(displayColor * clampedAlpha, clampedAlpha);
        }
        return float4(displayColor, 1.0f);
    }

    const float4 normalSample = gNormalTexture.Sample(gWrapSampler, uv);

    float3 normal = normalize(input.normal);
    if ((gMaterialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        const float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
        const float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        const float3 tangentNormal = normalSample.xyz * 2.0f - 1.0f;
        normal = normalize(tangent * tangentNormal.x +
                           bitangent * tangentNormal.y +
                           normal * tangentNormal.z);
    }
    else if ((gMaterialFlags & MATERIAL_MODEL_SPACE_NORMAL) != 0u)
    {
        normal = normalize(normalSample.xyz * 2.0f - 1.0f);
    }
    if (!isFrontFace)
    {
        normal = -normal;
    }

    const float hemisphereAmount = saturate(normal.z * 0.5f + 0.5f);
    float3 lighting = lerp(gAmbientBottom.rgb, gAmbientTop.rgb, hemisphereAmount);
    float3 specular = 0.0f;
    float specularMask = normalSample.a;
    if ((gMaterialFlags & MATERIAL_SPECULAR_MAP) != 0u)
    {
        specularMask = gSpecularTexture.Sample(gWrapSampler, uv).r;
    }

    [unroll]
    for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
    {
        const float3 lightDirection = normalize(gLightDirection[lightIndex].xyz);
        const float diffuseAmount = saturate(dot(normal, lightDirection));
        lighting += gLightColor[lightIndex].rgb * diffuseAmount;

        if ((gMaterialFlags & MATERIAL_SPECULAR) != 0u && diffuseAmount > 0.0f)
        {
            const float3 halfVector = normalize(lightDirection + viewDirection);
            const float highlight = pow(saturate(dot(normal, halfVector)),
                                        gSpecularColorPower.w);
            specular += gLightColor[lightIndex].rgb *
                        gSpecularColorPower.rgb *
                        highlight * specularMask * gMaterialParams.y * 0.85f;
        }
    }

    float3 environment = 0.0f;
    if ((gMaterialFlags & MATERIAL_ENVIRONMENT_MAP) != 0u)
    {
        const float3 reflection = reflect(-viewDirection, normal);
        // No Skyrim, quando nao ha mascara _m.dds explicita, o canal alpha do normal map
        // controla a intensidade do reflexo do cubemap.
        const float environmentMask = (gMaterialFlags & MATERIAL_ENVIRONMENT_MASK_MAP) != 0u
            ? gEnvironmentMaskTexture.Sample(gWrapSampler, uv).r
            : normalSample.a;
        // Piso minimo de 0.45 garante que metais escuros e vidro mantenham reflexo nitido
        // mesmo em areas fora da luz direta.
        const float envLightFade = (gMaterialFlags & MATERIAL_ENVMAP_LIGHT_FADE) != 0u
            ? lerp(0.45f, 1.0f, saturate(dot(lighting, float3(0.3333f, 0.3333f, 0.3333f))))
            : 1.0f;
        environment = gEnvironmentTexture.Sample(gClampSampler, reflection).rgb *
                      gSpecularColorPower.rgb * environmentMask * gMaterialParams.z *
                      envLightFade * 0.80f;
    }

    // An opaque Skyrim material may use diffuse alpha as data, not coverage.
    // Only an explicit alpha property permits it to cut/blend the silhouette.
    const float coverage = (gMaterialFlags & MATERIAL_TEXTURE_ALPHA) != 0u ? albedo.a : 1.0f;
    const float vertexAlpha = (gMaterialFlags & MATERIAL_VERTEX_ALPHA) != 0u ? input.color.a : 1.0f;
    const float alpha = saturate(coverage * gMaterialParams.x * vertexAlpha);
    if ((gMaterialFlags & MATERIAL_ALPHA_TEST) != 0u && alpha < gMaterialParams.w)
    {
        discard;
    }

    float3 diffuseColor = albedo.rgb * gTintColor.rgb;
    if ((gMaterialFlags & MATERIAL_FACEGEN) != 0u)
    {
        const float3 tintColor = gFaceTintTexture.Sample(gWrapSampler, uv).rgb;
        const float3 detailColor =
            (gFaceDetailTexture.Sample(gWrapSampler, uv).rgb +
             (1.0f / 255.0f)) * (255.0f / 64.0f);
        diffuseColor = ApplyFaceGenOverlay(albedo.rgb, tintColor, detailColor);
    }
    else if ((gMaterialFlags & MATERIAL_FACEGEN_RGB_TINT) != 0u)
    {
        const float3 detailColor = float3(1.01172f, 0.996094f, 1.01172f);
        diffuseColor = ApplyFaceGenOverlay(albedo.rgb, gTintColor.rgb, detailColor);
    }
    else if ((gMaterialFlags & MATERIAL_VERTEX_COLORS) != 0u)
    {
        diffuseColor *= SrgbToLinear(input.color.rgb);
    }

    float3 emissive = 0.0f;
    if ((gMaterialFlags & MATERIAL_GLOW_MAP) != 0u)
    {
        const float3 glowSample = gGlowTexture.Sample(gWrapSampler, uv).rgb;
        emissive = glowSample * gEmissiveColorScale.rgb * gEmissiveColorScale.w;
    }
    else if ((gMaterialFlags & MATERIAL_OWN_EMIT) != 0u)
    {
        emissive = diffuseColor * gEmissiveColorScale.rgb * gEmissiveColorScale.w;
    }

    if ((gMaterialFlags & MATERIAL_ADDITIVE_BLEND) != 0u)
    {
        const float3 addColor = EncodeLinearDisplay(diffuseColor * lighting + specular + environment + emissive);
        return float4(addColor * alpha, 0.0f);
    }
    if ((gMaterialFlags & MATERIAL_PREMULTIPLIED_ALPHA) != 0u)
    {
        // Em vidro e superficies translucidas: o Fresnel preserva o centro limpo para o
        // conteudo interno e destaca as bordas vitreas, enquanto o brilho especular estreito
        // reflete nitidamente sobre a superficie sem transformar o frasco em branco leitoso.
        const float ndotv = abs(dot(normal, viewDirection));
        const float fresnel = lerp(0.45f, 1.0f, pow(saturate(1.0f - ndotv), 2.0f));
        const float3 bodyColor = EncodeLinearDisplay(diffuseColor * lighting + environment * fresnel + emissive);
        const float3 specDisplay = EncodeLinearDisplay(specular);
        const float specLum = max(specDisplay.r, max(specDisplay.g, specDisplay.b));
        const float3 premultColor = saturate(bodyColor * alpha + specDisplay * (0.45f + 0.55f * alpha));
        const float outAlpha = saturate(alpha + specLum * 0.35f);
        return float4(premultColor, outAlpha);
    }
    const float3 litColor = diffuseColor * lighting + specular + environment + emissive;
    const float3 color = EncodeLinearDisplay(litColor);
    // Alpha-tested survivors are opaque. The compositor consumes a
    // premultiplied render target, including genuinely translucent draws.
    return float4(color, 1.0f);
}
