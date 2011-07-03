//--------------------------------------------------------------------------------
#include "App.h"
#include "Log.h"

#include "EventManager.h"
#include "EvtFrameStart.h"
#include "EvtChar.h"
#include "EvtKeyUp.h"
#include "EvtKeyDown.h"

#include "ScriptManager.h"

#include "SwapChainConfigDX11.h"
#include "Texture2dConfigDX11.h"

#include "GeometryLoaderDX11.h"
#include "GeometryGeneratorDX11.h"
#include "MaterialGeneratorDX11.h"
#include "RasterizerStateConfigDX11.h"

#include "IParameterManager.h"
#include "SamplerParameterWriterDX11.h"
#include "ShaderResourceParameterWriterDX11.h"
#include "DepthStencilStateConfigDX11.h"

#include "DepthStencilViewConfigDX11.h"
#include "ShaderResourceViewConfigDX11.h"

using namespace Glyph3;
//--------------------------------------------------------------------------------
App AppInstance; // Provides an instance of the application
//--------------------------------------------------------------------------------
Vector3f Lerp( const Vector3f& x, const Vector3f& y, const Vector3f& s )
{
    return x + s * ( y - x );
}
//--------------------------------------------------------------------------------
App::App() : m_vpWidth( 1024 ), m_vpHeight( 576 )
{
	m_bSaveScreenshot = false;
}
//--------------------------------------------------------------------------------
bool App::ConfigureEngineComponents()
{
	// Create the renderer and initialize it for the desired device
	// type and feature level.

	m_pRenderer11 = new RendererDX11();

	if ( !m_pRenderer11->Initialize( D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL_11_0 ) )
	{
		Log::Get().Write( L"Could not create hardware device, trying to create the reference device..." );

		if ( !m_pRenderer11->Initialize( D3D_DRIVER_TYPE_REFERENCE, D3D_FEATURE_LEVEL_11_0 ) )
		{
			MessageBox( m_pWindow->GetHandle(), L"Could not create a hardware or software Direct3D 11 device - the program will now abort!", L"Hieroglyph 3 Rendering", MB_ICONEXCLAMATION | MB_SYSTEMMODAL );
			RequestTermination();
			return( false );
		}

		// If using the reference device, utilize a fixed time step for any animations.
		m_pTimer->SetFixedTimeStep( 1.0f / 10.0f );
	}

    m_pRenderer11->SetMultiThreadingState( false );

	// Create the window wrapper class instance.
	m_pWindow = new Win32RenderWindow();
	m_pWindow->SetPosition( 50, 50 );
	m_pWindow->SetSize( m_vpWidth, m_vpHeight );
	m_pWindow->SetCaption( std::wstring( L"Direct3D 11 Window" ) );
	m_pWindow->Initialize();

	// Create a swap chain for the window.
	SwapChainConfigDX11 Config;
	Config.SetWidth( m_pWindow->GetWidth() );
	Config.SetHeight( m_pWindow->GetHeight() );
	Config.SetOutputWindow( m_pWindow->GetHandle() );
	m_pWindow->SetSwapChain( m_pRenderer11->CreateSwapChain( &Config ) );

	// We'll keep a copy of the swap chain's render target index to
	// use later.
	m_BackBuffer = m_pRenderer11->GetSwapChainResource( m_pWindow->GetSwapChain() );

    // Create render targets
    int rtWidth = m_vpWidth;
    int rtHeight = m_vpHeight;

    DXGI_SAMPLE_DESC sampleDesc;
    sampleDesc.Count =  4;
    sampleDesc.Quality = 0;

    // Create the render targets for our optimized G-Buffer
    Texture2dConfigDX11 RTConfig;
    RTConfig.SetColorBuffer( rtWidth, rtHeight );
    RTConfig.SetBindFlags( D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET );
    RTConfig.SetSampleDesc( sampleDesc );

    // For the G-Buffer we'll use 4-component floating point format for
    // spheremap-encoded normals, specular albedo, and the coverage mask
    RTConfig.SetFormat( DXGI_FORMAT_R16G16B16A16_FLOAT );
    m_GBufferTarget = m_pRenderer11->CreateTexture2D( &RTConfig, NULL );

    // For the light buffer we'll use a 4-component floating point format
    // for storing diffuse RGB + mono specular
    RTConfig.SetFormat( DXGI_FORMAT_R16G16B16A16_FLOAT );
    m_LightTarget = m_pRenderer11->CreateTexture2D( &RTConfig, NULL );

    // We need one last render target for the final image
    RTConfig.SetFormat( DXGI_FORMAT_R10G10B10A2_UNORM );
    m_FinalTarget = m_pRenderer11->CreateTexture2D( &RTConfig, NULL );

    // Next we create a depth buffer for depth/stencil testing. Typeless formats let us
    // write depth with one format, and later interpret that depth as a color value using
    // a shader resource view.
    Texture2dConfigDX11 DepthTexConfig;
    DepthTexConfig.SetDepthBuffer( rtWidth, rtHeight );
    DepthTexConfig.SetFormat( DXGI_FORMAT_R24G8_TYPELESS );
    DepthTexConfig.SetBindFlags( D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE );
    DepthTexConfig.SetSampleDesc( sampleDesc );

    DepthStencilViewConfigDX11 DepthDSVConfig;
    D3D11_TEX2D_DSV DSVTex2D;
    DSVTex2D.MipSlice = 0;
    DepthDSVConfig.SetTexture2D( DSVTex2D );
    DepthDSVConfig.SetFormat( DXGI_FORMAT_D24_UNORM_S8_UINT );
    DepthDSVConfig.SetViewDimensions( D3D11_DSV_DIMENSION_TEXTURE2DMS );

    ShaderResourceViewConfigDX11 DepthSRVConfig;
    D3D11_TEX2D_SRV SRVTex2D;
    SRVTex2D.MipLevels = 1;
    SRVTex2D.MostDetailedMip = 0;
    DepthSRVConfig.SetTexture2D( SRVTex2D );
    DepthSRVConfig.SetFormat( DXGI_FORMAT_R24_UNORM_X8_TYPELESS );
    DepthSRVConfig.SetViewDimensions( D3D11_SRV_DIMENSION_TEXTURE2DMS );

    m_DepthTarget = m_pRenderer11->CreateTexture2D( &DepthTexConfig, NULL, &DepthSRVConfig, NULL, NULL, &DepthDSVConfig );

    // Now we need to create a depth stencil view with read-only flags set, so
    // that we can have the same buffer set as both a shader resource view and as
    // a depth stencil view
    DepthDSVConfig.SetFlags( D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL );
    m_ReadOnlyDepthTarget = ResourcePtr( new ResourceProxyDX11( m_DepthTarget->m_iResource,
                                                                &DepthTexConfig, m_pRenderer11,
                                                                &DepthSRVConfig, NULL, NULL,
                                                                &DepthDSVConfig ) );

    // Create a view port to use on the scene.  This basically selects the
    // entire floating point area of the render target.
    D3D11_VIEWPORT viewport;
    viewport.Width = static_cast< float >( rtWidth );
    viewport.Height = static_cast< float >( rtHeight );
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;

    m_iViewport = m_pRenderer11->CreateViewPort( viewport );

    // Create a render target for MSAA resolve
    Texture2dConfigDX11 resolveConfig;
    resolveConfig.SetColorBuffer( m_vpWidth, m_vpHeight);
    resolveConfig.SetFormat( DXGI_FORMAT_R10G10B10A2_UNORM );
    resolveConfig.SetBindFlags( D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET );
    m_ResolveTarget = m_pRenderer11->CreateTexture2D( &resolveConfig, NULL );

	return( true );
}
//--------------------------------------------------------------------------------
void App::ShutdownEngineComponents()
{
	if ( m_pRenderer11 )
	{
		m_pRenderer11->Shutdown();
		delete m_pRenderer11;
	}

	if ( m_pWindow )
	{
		m_pWindow->Shutdown();
		delete m_pWindow;
	}
}
//--------------------------------------------------------------------------------
void App::Initialize()
{
	// Create and initialize the geometry to be rendered.
	GeometryDX11* pGeometry = new GeometryDX11();
	pGeometry = GeometryLoaderDX11::loadMS3DFile2( std::wstring( L"../Data/Models/Sample_Scene.ms3d" ) );
    bool success = pGeometry->ComputeTangentFrame();
    _ASSERT( success );
	pGeometry->LoadToBuffers();
	pGeometry->SetPrimitiveType( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	// Create an effect for filling the G-Buffer
    m_pGBufferEffect = new RenderEffectDX11();
    m_pGBufferEffect->m_iVertexShader =
        m_pRenderer11->LoadShader( VERTEX_SHADER,
        std::wstring( L"../Data/Shaders/GBufferLP.hlsl" ),
        std::wstring( L"VSMain" ),
        std::wstring( L"vs_5_0" ) );
    _ASSERT( m_pGBufferEffect->m_iVertexShader != -1 );
    m_pGBufferEffect->m_iPixelShader =
        m_pRenderer11->LoadShader( PIXEL_SHADER,
        std::wstring( L"../Data/Shaders/GBufferLP.hlsl" ),
        std::wstring( L"PSMain" ),
        std::wstring( L"ps_5_0" ) );
    _ASSERT( m_pGBufferEffect->m_iPixelShader != -1 );

    // Create an effect for rendering the final pass
    m_pFinalPassEffect = new RenderEffectDX11();
    m_pFinalPassEffect->m_iVertexShader =
        m_pRenderer11->LoadShader( VERTEX_SHADER,
        std::wstring( L"../Data/Shaders/FinalPassLP.hlsl" ),
        std::wstring( L"VSMain" ),
        std::wstring( L"vs_5_0" ) );
    _ASSERT( m_pFinalPassEffect->m_iVertexShader != -1 );
    m_pFinalPassEffect->m_iPixelShader =
        m_pRenderer11->LoadShader( PIXEL_SHADER,
        std::wstring( L"../Data/Shaders/FinalPassLP.hlsl" ),
        std::wstring( L"PSMain" ),
        std::wstring( L"ps_5_0" ) );
    _ASSERT( m_pFinalPassEffect->m_iPixelShader != -1 );

    // Create a depth stencil state for the G-Buffer effect. We'll enable depth
    // writes and depth tests. We'll also enable stencil writes, and set
    // the stencil buffer to 1 for each pixel rendered.
    DepthStencilStateConfigDX11 dsConfig;
    dsConfig.DepthEnable = true;
    dsConfig.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsConfig.DepthFunc = D3D11_COMPARISON_LESS;
    dsConfig.StencilEnable = true;
    dsConfig.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    dsConfig.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    dsConfig.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dsConfig.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dsConfig.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dsConfig.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dsConfig.BackFace = dsConfig.FrontFace;

    m_iGBufferDSState = m_pRenderer11->CreateDepthStencilState( &dsConfig );

    m_pGBufferEffect->m_iDepthStencilState = m_iGBufferDSState;
    m_pGBufferEffect->m_uStencilRef = 1;

    if ( m_iGBufferDSState == -1 )
        Log::Get().Write( L"Failed to create G-Buffer depth stencil state" );

    // Create a depth stencil state for the final pass effect. We'll disable
    // depth writes, but leave depth testing on. We'll also disable stencil testing
    dsConfig.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsConfig.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsConfig.StencilEnable = false;
    dsConfig.StencilWriteMask = 0;
    dsConfig.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dsConfig.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    dsConfig.BackFace = dsConfig.FrontFace;

    m_iFinalPassDSState = m_pRenderer11->CreateDepthStencilState( &dsConfig );

    m_pFinalPassEffect->m_iDepthStencilState = m_iFinalPassDSState;

    if ( m_iFinalPassDSState == -1 )
        Log::Get().Write( L"Failed to create final pass depth stencil state" );

    // Create a rasterizer state with back-face culling enabled
    RasterizerStateConfigDX11 rsConfig;
    rsConfig.MultisampleEnable = TRUE;
    rsConfig.CullMode = D3D11_CULL_BACK;
    m_iRSState = m_pRenderer11->CreateRasterizerState( &rsConfig );

    m_pGBufferEffect->m_iRasterizerState = m_iRSState;
    m_pFinalPassEffect->m_iRasterizerState = m_iRSState;

    if ( m_iRSState == -1 )
        Log::Get().Write( L"Failed to create rasterizer state" );

    // Create the material for rendering the geometry to the G-Buffer and the final pass
    m_pMaterial = new MaterialDX11();
    m_pMaterial->Params[VT_GBUFFER].pEffect = m_pGBufferEffect;
    m_pMaterial->Params[VT_GBUFFER].bRender = true;
    m_pMaterial->Params[VT_FINALPASS].pEffect = m_pFinalPassEffect;
    m_pMaterial->Params[VT_FINALPASS].bRender = false;

    // Load textures. For the diffuse map, we'll specify that we want an sRGB format so that
    // the texture data is gamma-corrected when sampled in the shader
    D3DX11_IMAGE_LOAD_INFO loadInfo;
    loadInfo.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    m_DiffuseTexture = m_pRenderer11->LoadTexture( std::wstring( L"../Data/Textures/Hex.png" ), &loadInfo );
    m_NormalMap = m_pRenderer11->LoadTexture( std::wstring( L"../Data/Textures/Hex_Normal.png" ) );

    _ASSERT( m_DiffuseTexture->m_iResource != -1 );
    _ASSERT( m_NormalMap->m_iResource != -1 );

    // Set the texture parameters
    ShaderResourceParameterWriterDX11* pDiffuseParam = new ShaderResourceParameterWriterDX11();
    pDiffuseParam->SetRenderParameterRef(
		m_pRenderer11->m_pParamMgr->GetShaderResourceParameterRef( std::wstring( L"DiffuseMap" ) ) );
	pDiffuseParam->SetValue( m_DiffuseTexture );
    m_pMaterial->AddRenderParameter( pDiffuseParam );

    ShaderResourceParameterWriterDX11* pNormalMapParam = new ShaderResourceParameterWriterDX11();
	pNormalMapParam->SetRenderParameterRef(
		m_pRenderer11->m_pParamMgr->GetShaderResourceParameterRef( std::wstring( L"NormalMap" ) ) );
	pNormalMapParam->SetValue( m_NormalMap );
    m_pMaterial->AddRenderParameter( pNormalMapParam );

    // Create a sampler state
    D3D11_SAMPLER_DESC sampDesc;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.BorderColor[0] = sampDesc.BorderColor[1] = sampDesc.BorderColor[2] = sampDesc.BorderColor[3] = 0;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    sampDesc.MaxAnisotropy = 16;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    sampDesc.MinLOD = 0.0f;
    sampDesc.MipLODBias = 0.0f;
    int samplerState = m_pRenderer11->CreateSamplerState( &sampDesc );

    // Create a sampler state parameter
    SamplerParameterWriterDX11* pSamplerParam = new SamplerParameterWriterDX11();
    pSamplerParam->SetRenderParameterRef(
		(RenderParameterDX11*)m_pRenderer11->m_pParamMgr->GetSamplerStateParameterRef( std::wstring( L"AnisoSampler" ) ) );
	pSamplerParam->SetValue( samplerState );
    m_pMaterial->AddRenderParameter( pSamplerParam );

	// Create the camera, and the render view that will produce an image of the
	// from the camera's point of view of the scene.
	m_pCamera = new Camera();
	m_pCamera->GetNode()->Rotation().Rotation( Vector3f( 0.407f, -0.707f, 0.0f ) );
	m_pCamera->GetNode()->Position() = Vector3f( 4.0f, 4.5f, -4.0f );

	m_pGBufferView = new ViewGBuffer( *m_pRenderer11 );

    const float nearClip = 1.0f;
    const float farClip = 15.0f;
	m_pCamera->SetProjectionParams( nearClip, farClip, (float)m_vpWidth / (float)m_vpHeight, (float)D3DX_PI / 2.0f );

    m_pLightsView = new ViewLights( *m_pRenderer11 );

    // Bind the light view to the camera entity
    m_pLightsView->SetEntity( m_pCamera->GetBody() );
    m_pLightsView->SetRoot( m_pCamera->GetNode() );
    m_pLightsView->SetProjMatrix( m_pCamera->ProjMatrix() );
    m_pLightsView->SetClipPlanes( nearClip, farClip );

    // Create the final pass view
    m_pFinalPassView = new ViewFinalPass( *m_pRenderer11 );

	// Create the scene and add the entities to it.  Then add the camera to the
	// scene so that it will be updated via the scene interface instead of
	// manually manipulating it.

	m_pNode = new Node3D();
	m_pEntity = new Entity3D();
	m_pEntity->SetGeometry( pGeometry );
	m_pEntity->SetMaterial( m_pMaterial, false );
	m_pEntity->Position() = Vector3f( 0.0f, 0.0f, 0.0f );

	m_pNode->AttachChild( m_pEntity );

	m_pScene->AddEntity( m_pNode );
	m_pScene->AddCamera( m_pCamera );

    m_SpriteRenderer.Initialize();
    m_Font.Initialize( L"Arial", 14, 0, true );

    // Set the the targets for the views
    m_pGBufferView->SetTargets( m_GBufferTarget,
                                m_DepthTarget,
                                m_iViewport );
    m_pLightsView->SetTargets( m_GBufferTarget,
                                m_LightTarget,
                                m_ReadOnlyDepthTarget,
                                m_iViewport );
    m_pFinalPassView->SetTargets( m_LightTarget,
                                    m_FinalTarget,
                                    m_ReadOnlyDepthTarget,
                                    m_iViewport );
}
//--------------------------------------------------------------------------------
void App::Update()
{
	// Update the timer to determine the elapsed time since last frame.  This can
	// then used for animation during the frame.

	m_pTimer->Update();

	// Create a series of time factors for use in the simulation.  The factors
	// are as follows:
	// x: Time in seconds since the last frame.
	// y: Current framerate, which is updated once per second.
	// z: Time in seconds since application startup.
	// w: Current frame number since application startup.

	Vector4f TimeFactors = Vector4f( m_pTimer->Elapsed(), (float)m_pTimer->Framerate(),
		m_pTimer->Runtime(), (float)m_pTimer->FrameCount() );

	m_pRenderer11->m_pParamMgr->SetVectorParameter( std::wstring( L"TimeFactors" ), &TimeFactors );

    SetupLights();

	// Send an event to everyone that a new frame has started.  This will be used
	// in later examples for using the material system with render views.

	EventManager::Get()->ProcessEvent( new EvtFrameStart( *m_pTimer ) );


	// Manipulate the scene here - simply rotate the root of the scene in this
	// example.

	Matrix3f rotation;
	rotation.RotationY( m_pTimer->Elapsed() * 0.2f );
	m_pNode->Rotation() *= rotation;

    // Set the camera and material to render the G-Buffer pass
    m_pCamera->SetCameraView( m_pGBufferView );
    m_pGBufferView->SetProjMatrix( m_pCamera->ProjMatrix() );
    m_pMaterial->Params[VT_GBUFFER].bRender = true;
    m_pMaterial->Params[VT_FINALPASS].bRender = false;

	// Update the scene, and then render it to the G-Buffer
	m_pScene->Update( m_pTimer->Elapsed() );
	m_pScene->Render( m_pRenderer11 );

    // Render the light pass
    m_pLightsView->PreDraw( m_pRenderer11 );
    m_pRenderer11->ProcessRenderViewQueue();

    // Set the render target for the final pass, and clear it
    PipelineManagerDX11* pImmPipeline = m_pRenderer11->pImmPipeline;
    pImmPipeline->ClearRenderTargets();
    pImmPipeline->BindRenderTargets( 0, m_FinalTarget );
    pImmPipeline->ApplyRenderTargets();
    pImmPipeline->ClearBuffers( Vector4f( 0.0f, 0.0f, 0.0f, 0.0f ) );

    // Also bind the depth buffer
    pImmPipeline->BindDepthTarget( m_DepthTarget );
    pImmPipeline->ApplyRenderTargets();

    // Now set the camera and material to render the final pass
    m_pCamera->SetCameraView( m_pFinalPassView );
    m_pFinalPassView->SetProjMatrix( m_pCamera->ProjMatrix() );
    m_pMaterial->Params[VT_GBUFFER].bRender = false;
    m_pMaterial->Params[VT_FINALPASS].bRender = true;
    m_pMaterial->Params[VT_FINALPASS].pEffect = m_pFinalPassEffect;
    m_pScene->Render( m_pRenderer11 );

    // Render to the backbuffer
    IParameterManager* pParams = m_pRenderer11->m_pParamMgr;
    pImmPipeline->ClearRenderTargets();
    pImmPipeline->BindRenderTargets( 0, m_BackBuffer );
    pImmPipeline->ApplyRenderTargets();
    pImmPipeline->ClearBuffers( Vector4f( 0.0f, 0.0f, 0.0f, 0.0f ) );

    // Need to resolve the MSAA target before we can render it
    pImmPipeline->ResolveSubresource( m_ResolveTarget, 0, m_FinalTarget, 0, DXGI_FORMAT_R10G10B10A2_UNORM );

    m_SpriteRenderer.Render( pImmPipeline, pParams, m_ResolveTarget, Matrix4f::Identity() );

    DrawHUD( );

	// Perform the rendering and presentation for the window.
	m_pRenderer11->Present( m_pWindow->GetHandle(), m_pWindow->GetSwapChain() );

	// Save a screenshot if desired.  This is done by pressing the 's' key, which
	// demonstrates how an event is sent and handled by an event listener (which
	// in this case is the application object itself).

	if ( m_bSaveScreenshot  )
	{
		m_bSaveScreenshot = false;
		m_pRenderer11->pImmPipeline->SaveTextureScreenShot( 0, std::wstring( L"LightPrepass_" ), D3DX11_IFF_BMP );
	}
}
//--------------------------------------------------------------------------------
void App::Shutdown()
{
	SAFE_DELETE( m_pEntity );
	SAFE_DELETE( m_pNode );
	SAFE_DELETE( m_pCamera );

	// Print the framerate out for the log before shutting down.

	std::wstringstream out;
	out << L"Max FPS: " << m_pTimer->MaxFramerate();
	Log::Get().Write( out.str() );
}
//--------------------------------------------------------------------------------
bool App::HandleEvent( IEvent* pEvent )
{
	eEVENT e = pEvent->GetEventType();

	if ( e == SYSTEM_KEYBOARD_KEYDOWN )
	{
		EvtKeyDown* pKeyDown = (EvtKeyDown*)pEvent;

		unsigned int key = pKeyDown->GetCharacterCode();

        if ( key == LightMode::Key )
        {
            LightMode::Increment();
            return true;
        }

		return( true );
	}
	else if ( e == SYSTEM_KEYBOARD_KEYUP )
	{
		EvtKeyUp* pKeyUp = (EvtKeyUp*)pEvent;

		unsigned int key = pKeyUp->GetCharacterCode();

		if ( key == VK_ESCAPE ) // 'Esc' Key - Exit the application
		{
			this->RequestTermination();
			return( true );
		}
		else if ( key == 0x53 ) // 'S' Key - Save a screen shot for the next frame
		{
			m_bSaveScreenshot = true;
			return( true );
		}
		else
		{
			return( false );
		}
	}

	// Call the parent class's event handler if we haven't handled the event.

	return( Application::HandleEvent( pEvent ) );
}
//--------------------------------------------------------------------------------
std::wstring App::GetName( )
{
	return( std::wstring( L"BasicApplication" ) );
}
//--------------------------------------------------------------------------------
void App::DrawHUD( )
{
    PipelineManagerDX11* pImmPipeline = m_pRenderer11->pImmPipeline;
    IParameterManager* pParams = m_pRenderer11->m_pParamMgr;

    Matrix4f transform = Matrix4f::Identity();
    transform.SetTranslation( Vector3f( 30.0f, 30.0f, 0.0f ) );
    std::wstring text = L"FPS: " + ToString( m_pTimer->Framerate() );
    m_SpriteRenderer.RenderText( pImmPipeline, pParams, m_Font, text.c_str(), transform );

    float x = 30.0f;
    float y = m_vpHeight - 100.0f;
    transform.SetTranslation( Vector3f( x, y, 0.0f ) );
    text = LightMode::ToString();
    m_SpriteRenderer.RenderText( pImmPipeline, pParams, m_Font, text.c_str(), transform );
}
//--------------------------------------------------------------------------------
void App::SetupLights( )
{
    // Set the lights to render
    Light light;
    light.Type = Point;
    light.Range = 2.0f;

    const int cubeSize = 3 + LightMode::Value * 2;
    const int cubeMin = -(cubeSize / 2);
    const int cubeMax = cubeSize / 2;

    const Vector3f minExtents ( -4.0f, 1.0f, -4.0f );
    const Vector3f maxExtents ( 4.0f, 11.0f, 4.0f );
    const Vector3f minColor ( 1.0f, 0.0f, 0.0f );
    const Vector3f maxColor ( 0.0f, 1.0f, 1.0f );

    for ( int x = cubeMin; x <= cubeMax; x++ )
    {
        for ( int y = cubeMin; y <= cubeMax; y++ )
        {
            for ( int z = cubeMin; z <= cubeMax; z++ )
            {
                Vector3f lerp;
                lerp.x = static_cast<float>( x - cubeMin ) / ( cubeSize - 1 );
                lerp.y = static_cast<float>( y - cubeMin ) / ( cubeSize - 1 );
                lerp.z = static_cast<float>( z - cubeMin ) / ( cubeSize - 1 );

                light.Position = Lerp( minExtents, maxExtents, lerp );
                light.Color = Lerp( minColor, maxColor, lerp ) * 1.5f;
                m_pLightsView->AddLight( light );
            }
        }
    }
}
//--------------------------------------------------------------------------------