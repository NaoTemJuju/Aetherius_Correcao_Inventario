#include "Render/NifCameraMath.h"

#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
    int g_failures = 0;

    void Expect(bool a_condition, const char* a_message)
    {
        if (!a_condition)
        {
            ++g_failures;
            std::cerr << "FAILED: " << a_message << '\n';
        }
    }

    bool Near(float a_left, float a_right)
    {
        return std::abs(a_left - a_right) < 0.0001f;
    }

    float Dot(const Meridian::Render::NifPreview::Float3& a_left,
              const Meridian::Render::NifPreview::Float3& a_right)
    {
        return a_left.x * a_right.x + a_left.y * a_right.y + a_left.z * a_right.z;
    }
}

int main()
{
    using namespace Meridian::Render::NifPreview;

    const auto front = BuildCameraBasis(0.0f, 0.0f);
    Expect(Near(front.eyeDirection.x, 1.0f) && Near(front.eyeDirection.y, 0.0f),
           "zero yaw places the eye on positive X");
    Expect(Near(front.right.x, 0.0f) && Near(front.right.y, 1.0f),
           "screen right is positive Y at zero yaw");
    Expect(Near(front.up.z, 1.0f), "screen up is positive Z at zero pitch");

    const auto quarterTurn = BuildCameraBasis(std::numbers::pi_v<float> * 0.5f, 0.0f);
    Expect(Near(quarterTurn.eyeDirection.x, 0.0f) && Near(quarterTurn.eyeDirection.y, 1.0f),
           "quarter-turn yaw places the eye on positive Y");
    Expect(Near(quarterTurn.right.x, -1.0f) && Near(quarterTurn.right.y, 0.0f),
           "screen right rotates with the camera");

    const auto pitched = BuildCameraBasis(0.4f, 0.3f);
    Expect(Near(Dot(pitched.eyeDirection, pitched.right), 0.0f),
           "eye and right vectors remain orthogonal");
    Expect(Near(Dot(pitched.eyeDirection, pitched.up), 0.0f),
           "eye and up vectors remain orthogonal");
    Expect(Near(Dot(pitched.right, pitched.up), 0.0f),
           "right and up vectors remain orthogonal");

    const Float3 center{10.0f, 20.0f, 30.0f};
    const auto panned = ApplyNormalizedPan(center, 8.0f, front, 0.5f, -0.25f, 0.125f);
    Expect(Near(panned.x, 11.0f), "depth pan scales by model radius");
    Expect(Near(panned.y, 24.0f), "horizontal pan scales by model radius");
    Expect(Near(panned.z, 28.0f), "vertical pan scales by model radius");

    // BSInvMarker: armadura deitada (+90 deg em X) deve apresentar a face +Z (peitoral)
    // voltada para a camera (-Y) e o eixo +Y (gola) para cima (+Z na tela).
    constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
    const auto cuirassQ = BuildInvMarkerQuaternion({kHalfPi, 0.0f, 0.0f});
    const auto cuirassChest = RotateVector(cuirassQ, {0.0f, 0.0f, 1.0f});
    const auto cuirassCollar = RotateVector(cuirassQ, {0.0f, 1.0f, 0.0f});
    Expect(Near(cuirassChest.x, 0.0f) && Near(cuirassChest.y, -1.0f) && Near(cuirassChest.z, 0.0f),
           "BSInvMarker 90 deg X faces armor chest (+Z) toward camera (-Y)");
    Expect(Near(cuirassCollar.x, 0.0f) && Near(cuirassCollar.y, 0.0f) && Near(cuirassCollar.z, 1.0f),
           "BSInvMarker 90 deg X points armor collar (+Y) straight up (+Z)");

    const auto cuirassBasis = CameraBasisFromObjectOrientation(cuirassQ);
    Expect(Near(cuirassBasis.eyeDirection.x, 0.0f) && Near(cuirassBasis.eyeDirection.y, 0.0f) &&
               Near(cuirassBasis.eyeDirection.z, 1.0f),
           "CameraBasisFromObjectOrientation places eye on +Z for 90 deg X cuirass");
    Expect(Near(cuirassBasis.up.x, 0.0f) && Near(cuirassBasis.up.y, 1.0f) &&
               Near(cuirassBasis.up.z, 0.0f),
           "CameraBasisFromObjectOrientation aligns screen up with +Y for 90 deg X cuirass");

    // BSInvMarker: tigela/ingrediente (45 deg X) mantem base ortonormal.
    const auto bowlBasis = CameraBasisFromObjectOrientation(
        BuildInvMarkerQuaternion({kHalfPi * 0.5f, 0.0f, 0.0f}));
    Expect(Near(Dot(bowlBasis.eyeDirection, bowlBasis.right), 0.0f) &&
               Near(Dot(bowlBasis.eyeDirection, bowlBasis.up), 0.0f) &&
               Near(Dot(bowlBasis.right, bowlBasis.up), 0.0f),
           "BSInvMarker 45 deg X produces orthonormal camera basis");

    // Trackball 360 graus livre (sem trava em +-75 deg e sem gimbal lock):
    // +dx puxa a frente (-Y) para a direita (+X); +dy puxa a frente (-Y) para baixo (-Z).
    const Quaternion identityQ{};
    const auto yawRightQ = ApplyScreenTrackballDelta(identityQ, kHalfPi, 0.0f);
    const auto frontAfterRight = RotateVector(yawRightQ, {0.0f, -1.0f, 0.0f});
    Expect(Near(frontAfterRight.x, 1.0f) && Near(frontAfterRight.y, 0.0f) &&
               Near(frontAfterRight.z, 0.0f),
           "positive horizontal drag turns object front (-Y) to screen right (+X)");

    const auto pitchDownQ = ApplyScreenTrackballDelta(identityQ, 0.0f, kHalfPi);
    const auto frontAfterDown = RotateVector(pitchDownQ, {0.0f, -1.0f, 0.0f});
    const auto topAfterDown = RotateVector(pitchDownQ, {0.0f, 0.0f, 1.0f});
    Expect(Near(frontAfterDown.x, 0.0f) && Near(frontAfterDown.y, 0.0f) &&
               Near(frontAfterDown.z, -1.0f),
           "positive vertical drag tilts object front (-Y) downward (-Z)");
    Expect(Near(topAfterDown.x, 0.0f) && Near(topAfterDown.y, -1.0f) &&
               Near(topAfterDown.z, 0.0f),
           "positive vertical drag brings object top (+Z) toward camera (-Y)");

    // 4 passos de 90 deg verticais (90 -> 180 de cabeca para baixo -> 270 -> 360)
    // devem girar livremente 360 graus e retornar ao ponto inicial.
    Quaternion spinQ = identityQ;
    for (int step = 0; step < 4; ++step)
    {
        spinQ = ApplyScreenTrackballDelta(spinQ, 0.0f, kHalfPi);
        const auto basis = CameraBasisFromObjectOrientation(spinQ);
        Expect(Near(Dot(basis.eyeDirection, basis.right), 0.0f) &&
                   Near(Dot(basis.eyeDirection, basis.up), 0.0f) &&
                   Near(Dot(basis.right, basis.up), 0.0f),
               "trackball basis stays orthonormal across full 360 vertical flip");
        if (step == 1)
        {
            const auto invertedTop = RotateVector(spinQ, {0.0f, 0.0f, 1.0f});
            Expect(Near(invertedTop.z, -1.0f),
                   "180 deg vertical drag flips object completely upside down without pitch clamp");
        }
    }
    const auto restoredTop = RotateVector(spinQ, {0.0f, 0.0f, 1.0f});
    const auto restoredFront = RotateVector(spinQ, {0.0f, -1.0f, 0.0f});
    Expect(Near(restoredTop.z, 1.0f) && Near(restoredFront.y, -1.0f),
           "360 deg vertical trackball rotation returns smoothly to start orientation");

    if (g_failures != 0)
    {
        std::cerr << g_failures << " NIF camera math test(s) failed\n";
        return 1;
    }

    std::cout << "All NIF camera math tests passed\n";
    return 0;
}
