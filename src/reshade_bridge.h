#pragma once
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

// [RESHADE] Bridge to a user-installed ReShade (add-on support build). Detects ReShade in-process,
// registers this module as an addon, holds the effect runtime, and renders the user's enabled
// techniques onto each OpenXR eye image at submit. No ReShade present -> fully inert (the free-mod
// tolerance guardrail from the monetization model). Not called for the menu swapchain.
namespace MELEVR::ReShadeBridge
{
void TickRegistration() noexcept;   // bounded retry; call ~once per worker-thread iteration
bool Detected() noexcept;           // a ReShade addon module is loaded (drives menu visibility)
bool RuntimeReady() noexcept;       // registered AND an effect runtime is live
const char* StatusLine() noexcept;  // "" when healthy; else a short reason for the menu
void SetEnabled(bool on) noexcept;  // wired to the reshadeInHeadset config each frame
void ApplyToImage(ID3D11Texture2D* image) noexcept;                 // per-eye color effects
void BindDepth(ID3D11ShaderResourceView* depthSrv) noexcept;        // stage 2: DEPTH semantic

// [RESHADE] in-headset shader control: enumerate the loaded techniques and toggle them from a dedicated
// head-locked VR menu, so the desktop-only ReShade overlay is not needed to configure effects in VR.
void RefreshTechniques() noexcept;                 // re-enumerate techniques + params; call while the menu is open
int  TechniqueCount() noexcept;
const char* TechniqueName(int index) noexcept;     // "" if out of range
bool TechniqueEnabled(int index) noexcept;
void SetTechniqueEnabled(int index, bool on) noexcept;

// Uniform parameters (the actual VALUES/sliders). Caller renders generic widgets; no ReShade types leak.
int  ParamCount() noexcept;                        // valid after RefreshTechniques()
const char* ParamName(int index) noexcept;
int  ParamKind(int index) noexcept;                // 0=bool, 1=int, 2=float
int  ParamComponents(int index) noexcept;          // 1..4 (float2/3/4 etc.)
float ParamMin(int index) noexcept;
float ParamMax(int index) noexcept;
void ParamGetFloat(int index, float* out4) noexcept;    // fills [ParamComponents]
void ParamSetFloat(int index, const float* v) noexcept;
int  ParamGetInt(int index) noexcept;
void ParamSetInt(int index, int v) noexcept;
bool ParamGetBool(int index) noexcept;
void ParamSetBool(int index, bool v) noexcept;
}
