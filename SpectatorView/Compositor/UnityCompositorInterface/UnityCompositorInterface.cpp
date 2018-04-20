// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "stdafx.h"

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"

#include "Network.h"

#include <ppltasks.h>
#include <DirectXMath.h>

#include "DirectXHelper.h"
#include "CompositorInterface.h"

#include "NetworkPacketStructure.h"

#include <mutex>

using namespace concurrency;
using namespace DirectX;

#define UNITYDLL EXTERN_C __declspec(dllexport)

char* SpectatorViewIP = nullptr;
static bool connectedToServer = false;
static bool connectingToServer = false;
WSASession Session;
static TCPSocket* tcp = nullptr;

static IUnityInterfaces* s_UnityInterfaces = NULL;
static IUnityGraphics* s_Graphics = NULL;
static UnityGfxRenderer s_RendererType = kUnityGfxRendererNull;

static ID3D11Device* g_pD3D11Device = NULL;
static ID3D11DeviceContext* g_pD3DContext = NULL;

LONGLONG additionalOffsetTime = 0;
SVPose svPose;
byte* poseBytes = new byte[sizeof(SVPose)];

ClientToServerPacket sentData;

static CompositorInterface* ci = NULL;

static bool isRecording = false;
static bool videoInitialized = false;

static LONGLONG colorTime = INVALID_TIMESTAMP;

static bool takePicture = false;

// Textures:
// yuv or rgba depending on capture pipeline.
static ID3D11Texture2D* g_colorTexture = nullptr;
static ID3D11ShaderResourceView* g_UnityColorSRV = nullptr;
// Must match format from input color texture - eg: composited frame converted back to yuv.
static ID3D11Texture2D* g_outputTexture = nullptr;
// Composited frame converted to NV12 for hardware encoded video.
static ID3D11Texture2D* g_videoTexture = nullptr;

std::mutex renderlock;

void ListenForServer()
{
    if (connectingToServer || connectedToServer)
    {
        return;
    }

    create_task([=]
    {
        while (true)
        {
            connectingToServer = true;
            if (!connectedToServer && SpectatorViewIP != nullptr && tcp != nullptr)
            {
                connectedToServer = tcp->CreateClientListener(SpectatorViewIP);
            }

            if (connectedToServer)
            {
                connectingToServer = false;
                return;
            }

            // Sleep so we do not starve the CPU.
            Sleep(100);
        }
    });
}

void ListenForData()
{
    if (connectedToServer && tcp != nullptr)
    {
        if (tcp->ReceiveData(poseBytes, sizeof(SVPose)))
        {
            memcpy(&svPose, poseBytes, sizeof(SVPose));
            if (ci != nullptr)
            {
                ci->AddPoseToPoseCache(
                    XMFLOAT3(svPose.posX, svPose.posY, svPose.posZ),
                    XMFLOAT4(svPose.rotX, svPose.rotY, svPose.rotZ, svPose.rotW),
                    (float)(0.0001f * svPose.sentTime / 1000.0f)
                );
            }
        }
        else
        {
            // Connection has ended, wait for SV to come back online.
            connectedToServer = false;
            ListenForServer();
        }
    }
}

UNITYDLL bool SetAnchorData(const char* ip, int anchorPort, const char* anchorName)
{
    if (!connectedToServer || tcp == nullptr)
    {
        return false;
    }

    memcpy(&sentData.anchorOwnerIP[0], ip, strlen(ip));
    sentData.anchorIPLength = (int)strlen(ip);
    
    sentData.anchorPort = anchorPort;

    int nameLength = (int)strlen(anchorName);
    // We only have 32 bytes for the name.
    if (nameLength > 32) { nameLength = 32; }

    memcpy(&sentData.anchorName[0], anchorName, nameLength);
    sentData.anchorNameLength = nameLength;

    sentData.forceAnchorReconnect = false;

    concurrency::create_task([=]
    {
        tcp->SendData((byte*)&sentData, sizeof(ClientToServerPacket));
    });

    return true;
}

UNITYDLL bool ForceAnchorReconnect()
{
    if (!connectedToServer || tcp == nullptr)
    {
        return false;
    }

    sentData.forceAnchorReconnect = true;
    
    concurrency::create_task([=]
    {
        tcp->SendData((byte*)&sentData, sizeof(ClientToServerPacket));
    });

    return true;
}

UNITYDLL void SetSpectatorViewIP(const char* ip)
{
    SpectatorViewIP = new char[strlen(ip) + 1];
    strcpy_s(SpectatorViewIP, strlen(ip) + 1, ip);
}

UNITYDLL void GetPose(XMFLOAT3& pos, XMFLOAT4& rot, float UnityTimeS, int frameOffset)
{
    if (ci == nullptr)
    {
        pos = XMFLOAT3(0, 0, 0);
        rot = XMFLOAT4(0, 0, 0, 1);
        
        return;
    }

    ci->GetPose(pos, rot, UnityTimeS, frameOffset);
}

// Plugin function to handle a specific rendering event
static void UNITY_INTERFACE_API OnRenderEvent(int eventID)
{
    if (ci == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(renderlock);
    if (g_pD3D11Device != nullptr)
    {
        ci->UpdateFrameProvider();

        if (!videoInitialized && ci != nullptr)
        {
            videoInitialized = ci->InitializeVideoEncoder(g_pD3D11Device);
        }

        // Photo
        if (takePicture)
        {
            takePicture = false;
            ci->TakePicture(g_outputTexture);
        }

        // Video
        if (isRecording &&
            g_videoTexture != nullptr)
        {
            ci->UpdateVideoRecordingFrame(g_videoTexture);
        }
    }
}

#pragma region
UNITYDLL void SetAudioData(BYTE* audioData)
{
    if (!isRecording)
    {
        return;
    }

#if ENCODE_AUDIO
    // Get the time for the audio frame.
    LARGE_INTEGER time;
    QueryPerformanceCounter(&time);

    if (ci != nullptr)
    {
        ci->RecordAudioFrameAsync(audioData, time.QuadPart);
    }
#endif
}

UNITYDLL void TakePicture()
{
    takePicture = true;
}

UNITYDLL void StartRecording()
{
    if (videoInitialized && ci != nullptr)
    {
        ci->StartRecording();
        isRecording = true;
    }
}

UNITYDLL void StopRecording()
{
    if (videoInitialized && ci != nullptr)
    {
        ci->StopRecording();
        isRecording = false;
    }
}

UNITYDLL bool IsRecording()
{
    return isRecording;
}
#pragma endregion Recording

#pragma region
static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
    switch (eventType)
    {
    case kUnityGfxDeviceEventInitialize:
    {
        s_RendererType = s_Graphics->GetRenderer();
        //TODO: user initialization code
        IUnityGraphicsD3D11* d3d11 = s_UnityInterfaces->Get<IUnityGraphicsD3D11>();
        if (d3d11 != nullptr)
        {
            g_pD3D11Device = d3d11->GetDevice();
            g_pD3D11Device->GetImmediateContext(&g_pD3DContext);
        }
        break;
    }
    case kUnityGfxDeviceEventShutdown:
    {
        g_pD3D11Device = NULL;
        s_RendererType = kUnityGfxRendererNull;
        //TODO: user shutdown code
        break;
    }
    case kUnityGfxDeviceEventBeforeReset:
    {
        //TODO: user Direct3D code
        break;
    }
    case kUnityGfxDeviceEventAfterReset:
    {
        //TODO: user Direct3D code
        break;
    }
    };
}

// Freely defined function to pass a callback to plugin-specific scripts
extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventFunc()
{
    return OnRenderEvent;
}

// Unity plugin load event
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    s_UnityInterfaces = unityInterfaces;
    s_Graphics = unityInterfaces->Get<IUnityGraphics>();

    s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

    // Run OnGraphicsDeviceEvent(initialize) manually on plugin load
    // to not miss the event in case the graphics device is already initialized
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

UNITYDLL void ResetSV()
{
    std::lock_guard<std::mutex> lock(renderlock);

    g_colorTexture = nullptr;
    g_UnityColorSRV = nullptr;
    g_outputTexture = nullptr;
    g_videoTexture = nullptr;
}

// Unity plugin unload event
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    ResetSV();

    s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
}
#pragma endregion Setup

#pragma region
UNITYDLL bool SetOutputRenderTexture(ID3D11Texture2D* tex)
{
    if (g_outputTexture == nullptr)
    {
        g_outputTexture = tex;
    }

    return g_outputTexture != nullptr;
}

UNITYDLL bool SetVideoRenderTexture(ID3D11Texture2D* tex)
{
    if (g_videoTexture == nullptr)
    {
        g_videoTexture = tex;
    }

    return g_videoTexture != nullptr;
}

UNITYDLL bool CreateUnityColorTexture(ID3D11ShaderResourceView*& srv)
{
    if (g_UnityColorSRV == nullptr && g_pD3D11Device != nullptr)
    {
        g_colorTexture = DirectXHelper::CreateTexture(g_pD3D11Device, FRAME_WIDTH, FRAME_HEIGHT, FRAME_BPP);

        if (g_colorTexture == nullptr)
        {
            return false;
        }

        g_UnityColorSRV = DirectXHelper::CreateShaderResourceView(g_pD3D11Device, g_colorTexture);
        if (g_UnityColorSRV == nullptr)
        {
            return false;
        }
    }

    srv = g_UnityColorSRV;
    return true;
}
#pragma endregion CreateExternalTextures

UNITYDLL bool InitializeFrameProvider()
{
    if (g_outputTexture == nullptr ||
        g_UnityColorSRV == nullptr ||
        g_pD3D11Device == nullptr)
    {
        return false;
    }

    if (tcp == nullptr)
    {
        tcp = new TCPSocket();

        ListenForServer();

        concurrency::create_task([=]
        {
            while (true)
            {
                ListenForData();
            }
        });
    }

    if (ci == nullptr)
    {
        ci = new CompositorInterface();
    }

    bool setColorSRV = false;
    if (ci != nullptr)
    {
        setColorSRV = ci->Initialize(g_pD3D11Device, g_UnityColorSRV, g_outputTexture);
    }

    return ci != nullptr && setColorSRV;
}

UNITYDLL void StopFrameProvider()
{
    if (ci != NULL)
    {
        ci->StopFrameProvider();
    }
}

UNITYDLL void UpdateCompositor()
{
    if (ci == NULL)
    {
        return;
    }

    ci->Update();

    LONGLONG cachedTime = colorTime;
    colorTime = ci->GetTimestamp(ci->GetCaptureFrameIndex());
}

UNITYDLL int GetFrameWidth()
{
    return HOLOGRAM_WIDTH;
}

UNITYDLL int GetFrameHeight()
{
    return HOLOGRAM_HEIGHT;
}

UNITYDLL bool OutputYUV()
{
    if (ci == NULL)
    {
        return true;
    }

    return ci->OutputYUV();
}

UNITYDLL void ResetPoseCache()
{
    if (ci != NULL)
    {
        ci->ResetPoseCache();
    }
}
