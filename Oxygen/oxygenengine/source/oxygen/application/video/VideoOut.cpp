/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/EngineMain.h"
#include "oxygen/drawing/opengl/OpenGLDrawer.h"
#include "oxygen/drawing/opengl/OpenGLDrawerTexture.h"
#include "oxygen/helper/Logging.h"
#include "oxygen/rendering/Geometry.h"
#include "oxygen/rendering/RenderResources.h"
#include "oxygen/rendering/opengl/OpenGLRenderer.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/rendering/software/SoftwareRenderer.h"
#include "oxygen/resources/FontCollection.h"
#include "oxygen/simulation/CodeExec.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/LemonScriptRuntime.h"
#include "oxygen/simulation/LogDisplay.h"
#include "oxygen/application/Application.h"
#include "oxygen/simulation/Simulation.h"


VideoOut::VideoOut() :
	mRenderResources(*new RenderResources())
{
	mGeometries.reserve(0x100);
}

VideoOut::~VideoOut()
{
	delete mRenderParts;
	delete &mRenderResources;
	delete mSoftwareRenderer;
#ifdef RMX_WITH_OPENGL_SUPPORT
	delete mOpenGLRenderer;
#endif
}

void VideoOut::startup()
{
	mGameResolution = Configuration::instance().mGameScreen;

	RMX_LOG_INFO("VideoOut: Setup of game screen");
	mGameScreenTexture.setupAsRenderTarget(mGameResolution);

	if (nullptr == mRenderParts)
	{
		RMX_LOG_INFO("VideoOut: Creating render parts");
		mRenderParts = new RenderParts();
	}

	createRenderer(false);
}

void VideoOut::shutdown()
{
	clearGeometries();
}

void VideoOut::reset()
{
	mRenderParts->reset();
	mActiveRenderer->reset();

	mFrameInterpolation.mUseInterpolationLastUpdate = false;
	mFrameInterpolation.mUseInterpolationThisUpdate = false;
	mDebugDrawRenderingRequested = false;
	mPreviouslyHadNewRenderItems = false;
}

void VideoOut::handleActiveModsChanged()
{
	// Better reset everything, as sprite references might have become invalid and should be removed
	reset();
}

void VideoOut::createRenderer(bool reset)
{
	setActiveRenderer(Configuration::instance().mRenderMethod == Configuration::RenderMethod::OPENGL_FULL, reset);
}

void VideoOut::destroyRenderer()
{
	SAFE_DELETE(mSoftwareRenderer);
#ifdef RMX_WITH_OPENGL_SUPPORT
	SAFE_DELETE(mOpenGLRenderer);
#endif
}

void VideoOut::setActiveRenderer(bool useOpenGLRenderer, bool reset)
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (useOpenGLRenderer)
	{
		if (nullptr == mOpenGLRenderer)
		{
			RMX_LOG_INFO("VideoOut: Creating OpenGL renderer");
			mOpenGLRenderer = new OpenGLRenderer(*mRenderParts, mGameScreenTexture);

			RMX_LOG_INFO("VideoOut: Renderer initialization");
			mOpenGLRenderer->initialize();
		}
		mActiveRenderer = mOpenGLRenderer;
	}
	else
#endif
	{
		if (nullptr == mSoftwareRenderer)
		{
			RMX_LOG_INFO("VideoOut: Creating software renderer");
			mSoftwareRenderer = new SoftwareRenderer(*mRenderParts, mGameScreenTexture);

			RMX_LOG_INFO("VideoOut: Renderer initialization");
			mSoftwareRenderer->initialize();
		}
		mActiveRenderer = mSoftwareRenderer;
	}

	if (reset)
	{
		mActiveRenderer->reset();
		mActiveRenderer->setGameResolution(mGameResolution);
	}
}

void VideoOut::setScreenSize(uint32 width, uint32 height)
{
	mGameResolution.x = width;
	mGameResolution.y = height;

	mGameScreenTexture.setupAsRenderTarget(mGameResolution);

	mActiveRenderer->setGameResolution(mGameResolution);

	// Render game screen again (this is particularly needed when switching from in-game Options back to the Pause Menu)
	mRequireGameScreenUpdate = true;
}

Vec2i VideoOut::getInterpolatedWorldSpaceOffset() const
{
	Vec2i offset = mRenderParts->getSpacesManager().getWorldSpaceOffset();
	if (mFrameInterpolation.mCurrentlyInterpolating)
	{
		const Vec2f interpolatedDifference = Vec2f(mLastWorldSpaceOffset - offset) * (1.0f - mFrameInterpolation.mInterFramePosition);
		offset += Vec2i(roundToInt(interpolatedDifference.x), roundToInt(interpolatedDifference.y));
	}
	return offset;
}

void VideoOut::preFrameUpdate()
{
	mRenderParts->preFrameUpdate();
	mLastWorldSpaceOffset = mRenderParts->getSpacesManager().getWorldSpaceOffset();

	// Skipped frames without rendering?
	if (mFrameState == FrameState::FRAME_READY)
	{
		// Processing of last frame (to avoid e.g. sprites rendered multiple times)
		RefreshParameters refreshParameters;
		refreshParameters.mSkipThisFrame = true;
		mRenderParts->refresh(refreshParameters);
	}

	mFrameState = FrameState::INSIDE_FRAME;
	mDebugDrawRenderingRequested = false;
}

void VideoOut::postFrameUpdate()
{
	mRenderParts->postFrameUpdate();

	// Signal for rendering
	mFrameState = FrameState::FRAME_READY;
	mLastFrameTicks = SDL_GetTicks();
	mFrameInterpolation.mUseInterpolationLastUpdate = mFrameInterpolation.mUseInterpolationThisUpdate;
	mFrameInterpolation.mUseInterpolationThisUpdate = true;		// Could be set differently, e.g. if we had a script binding to disable interpolation for an update
	mDebugDrawRenderingRequested = false;
}

void VideoOut::initAfterSaveStateLoad()
{
	mFrameState = FrameState::FRAME_READY;
	mLastFrameTicks = SDL_GetTicks();
	mFrameInterpolation.mUseInterpolationThisUpdate = false;
	mDebugDrawRenderingRequested = false;
}

void VideoOut::setInterFramePosition(float position)
{
	mFrameInterpolation.mInterFramePosition = position;
}

bool VideoOut::updateGameScreen()
{
	mFrameInterpolation.mCurrentlyInterpolating = (Configuration::instance().mFrameSync == Configuration::FrameSyncType::FRAME_INTERPOLATION && mFrameInterpolation.mUseInterpolationLastUpdate && mFrameInterpolation.mUseInterpolationThisUpdate);

	// Only render something if a frame simulation was completed in the meantime
	const bool hasNewSimulationFrame = (mFrameState == FrameState::FRAME_READY);
	if (!hasNewSimulationFrame && !mFrameInterpolation.mCurrentlyInterpolating && !mDebugDrawRenderingRequested && !mRequireGameScreenUpdate)
	{
		// No update
		return false;
	}

	mFrameState = FrameState::OUTSIDE_FRAME;
	mRequireGameScreenUpdate = false;

	RefreshParameters refreshParameters;
	refreshParameters.mSkipThisFrame = false;
	refreshParameters.mHasNewSimulationFrame = hasNewSimulationFrame;
	refreshParameters.mUsingFrameInterpolation = mFrameInterpolation.mCurrentlyInterpolating;
	refreshParameters.mInterFramePosition = mFrameInterpolation.mInterFramePosition;
	mRenderParts->refresh(refreshParameters);

	// Render a new image
	renderGameScreen();

	// Game screen got updated
	return true;
}

void VideoOut::blurGameScreen()
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (mActiveRenderer == mOpenGLRenderer)
	{
		mOpenGLRenderer->blurGameScreen();
	}
#endif
}

void VideoOut::toggleLayerRendering(int index)
{
	mRenderParts->mLayerRendering[index] = !mRenderParts->mLayerRendering[index];
}

std::string VideoOut::getLayerRenderingDebugString() const
{
	char string[10] = "basc BASC";
	for (int i = 0; i < 8; ++i)
	{
		if (!mRenderParts->mLayerRendering[i])
			string[i + i/4] = '-';
	}
	return string;
}

void VideoOut::getScreenshot(Bitmap& outBitmap)
{
	mGameScreenTexture.writeContentToBitmap(outBitmap);
}

DrawerTexture& VideoOut::getActiveDisplayTexture()
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (Configuration::instance().mStereoEyeSeparation > 0 && mStereoBuffersReady)
		return mStereoTexture;
#endif
	return mGameScreenTexture;
}

Vec2i VideoOut::getActiveDisplaySize() const
{
	if (Configuration::instance().mStereoEyeSeparation > 0)
		return Vec2i(mGameResolution.x * 2, mGameResolution.y);
	return mGameResolution;
}

void VideoOut::clearGeometries()
{
	for (Geometry* geometry : mGeometries)
	{
		mGeometryFactory.destroy(*geometry);
	}
	mGeometries.clear();

	// Regularly cleanup old cache items -- it's safe now that no geometry references a texture in there any more
	RenderResources::instance().mPrintedTextCache.regularCleanup();
}

void VideoOut::collectGeometries(std::vector<Geometry*>& geometries)
{
	// Add plane geometries
	{
		const PlaneManager& pm = mRenderParts->getPlaneManager();
		const Recti fullscreenRect(0, 0, mGameResolution.x, mGameResolution.y);

		static std::vector<PlaneManager::PlaneRect> planeRects;
		pm.getPlaneRects(planeRects, fullscreenRect);

		// Render default planes
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool priorityFlag = (pass == 1);

			for (const PlaneManager::PlaneRect& planeRect : planeRects)
			{
				// Note that layer rendering flags and default plane enabled flags don't properly support differentiation between plane A and W - this needs a rework eventually
				const bool isPlaneB = (planeRect.mPlane == PlaneManager::PLANE_B);
				const int layerIndex = (isPlaneB ? 0 : 1) + (priorityFlag ? 4 : 0);
				const int defaultPlaneIndex = (isPlaneB ? 0 : 1) + (priorityFlag ? 2 : 0);

				if (mRenderParts->mLayerRendering[layerIndex] && pm.isDefaultPlaneEnabled(defaultPlaneIndex))
				{
					uint8 scrollOffsets = (uint8)planeRect.mPlane;
					int renderQueue;
					switch (planeRect.mPlane)
					{
						default:
						case PlaneManager::PLANE_B:  renderQueue = priorityFlag ? 0x3000 : 0x1000;  break;
						case PlaneManager::PLANE_A:  renderQueue = priorityFlag ? 0x4000 : 0x2000;  break;
						case PlaneManager::PLANE_W:  renderQueue = priorityFlag ? 0x4200 : 0x2200;  scrollOffsets = 0xff;  break;
					}

					geometries.push_back(&mGeometryFactory.createPlaneGeometry(planeRect.mRect, planeRect.mPlane, priorityFlag, scrollOffsets, renderQueue));
				}
			}
		}

		// Render custom planes
		if (!pm.getCustomPlanes().empty())
		{
			for (const auto& customPlane : pm.getCustomPlanes())
			{
				geometries.push_back(&mGeometryFactory.createPlaneGeometry(customPlane.mRect, customPlane.mSourcePlane & 0x03, (customPlane.mSourcePlane & 0x10) != 0, customPlane.mScrollOffsets, customPlane.mRenderQueue));
			}
		}
	}

	// Add render item geometries (sprites, texts, etc.)
	{
		SpriteManager& spriteManager = mRenderParts->getSpriteManager();
		const Vec2i worldSpaceOffset = mRenderParts->getSpacesManager().getWorldSpaceOffset();
		FontCollection& fontCollection = FontCollection::instance();

		for (int index = 0; index < RenderItem::NUM_LIFETIME_CONTEXTS; ++index)
		{
			const RenderItem::LifetimeContext lifetimeContext = (RenderItem::LifetimeContext)index;
			const std::vector<RenderItem*>& renderItems = spriteManager.getRenderItems(lifetimeContext);

			for (RenderItem* renderItem : renderItems)
			{
				switch (renderItem->getType())
				{
					case RenderItem::Type::RECTANGLE:
					{
						const renderitems::Rectangle& rectangle = static_cast<const renderitems::Rectangle&>(*renderItem);

						Color color = rectangle.mColor;
						if (rectangle.mUseGlobalComponentTint)
						{
							mRenderParts->getPaletteManager().applyGlobalComponentTint(color);
						}

						Geometry& geometry = mGeometryFactory.createRectGeometry(Recti(rectangle.mPosition, rectangle.mSize), color);
						geometry.mRenderQueue = rectangle.mRenderQueue;
						geometries.push_back(&geometry);
						break;
					}

					case RenderItem::Type::TEXT:
					{
						const renderitems::Text& text = static_cast<const renderitems::Text&>(*renderItem);

						Font* font = fontCollection.getFontByKey(text.mFontKeyHash);
						if (nullptr == font)
						{
							font = fontCollection.createFontByKey(text.mFontKeyString);
						}
						if (nullptr != font)
						{
							const PrintedTextCache::Key key(text.mFontKeyHash, text.mTextHash, (uint8)text.mSpacing);
							PrintedTextCache& cache = RenderResources::instance().mPrintedTextCache;
							PrintedTextCache::CacheItem* cacheItem = cache.getCacheItem(key);
							if (nullptr == cacheItem)
							{
								cacheItem = &cache.addCacheItem(key, *font, text.mTextString);
							}
							const Vec2i drawPosition = Font::applyAlignment(Recti(text.mPosition, Vec2i(0, 0)), cacheItem->mInnerRect, text.mAlignment);
							const Recti rect(drawPosition, cacheItem->mTexture.getSize());

							Color tintColor = text.mColor;
							Color addedColor = Color::TRANSPARENT;
							if (text.mUseGlobalComponentTint)
							{
								mRenderParts->getPaletteManager().applyGlobalComponentTint(tintColor, addedColor);
							}

							Geometry& geometry = mGeometryFactory.createTexturedRectGeometry(rect, cacheItem->mTexture, tintColor, addedColor);
							geometry.mRenderQueue = text.mRenderQueue;
							geometries.push_back(&geometry);
						}
						break;
					}

					case RenderItem::Type::VDP_SPRITE:
					case RenderItem::Type::PALETTE_SPRITE:
					case RenderItem::Type::COMPONENT_SPRITE:
					case RenderItem::Type::SPRITE_MASK:
					{
						renderitems::SpriteInfo& sprite = static_cast<renderitems::SpriteInfo&>(*renderItem);
						bool accept = true;
						switch (renderItem->getType())
						{
							case RenderItem::Type::VDP_SPRITE:
							{
								accept = (mRenderParts->mLayerRendering[sprite.mPriorityFlag ? 6 : 2]);
								break;
							}

							case RenderItem::Type::PALETTE_SPRITE:
							case RenderItem::Type::COMPONENT_SPRITE:
							{
								accept = (mRenderParts->mLayerRendering[sprite.mPriorityFlag ? 7 : 3]);
								break;
							}

							default:
								// Accept everything else
								break;
						}

						if (accept)
						{
							sprite.mInterpolatedPosition = sprite.mPosition;
							if (mFrameInterpolation.mCurrentlyInterpolating)
							{
								Vec2i difference;
								if (sprite.mHasLastPosition)
								{
									difference = sprite.mLastPositionChange;
								}
								else if (sprite.mLogicalSpace == SpriteManager::Space::WORLD)
								{
									// Assume sprite is standing still in world space, i.e. moving entirely with camera
									difference = mLastWorldSpaceOffset - worldSpaceOffset;
								}
								else
								{
									// Assume sprite is standing still in screen space, i.e. not moving on the screen
								}

								if ((difference.x != 0 || difference.y != 0) && (abs(difference.x) < 0x40 && abs(difference.y) < 0x40))
								{
									const Vec2f interpolatedDifference = Vec2f(difference) * (1.0f - mFrameInterpolation.mInterFramePosition);
									sprite.mInterpolatedPosition -= Vec2i(roundToInt(interpolatedDifference.x), roundToInt(interpolatedDifference.y));
								}
							}

							SpriteGeometry& spriteGeometry = mGeometryFactory.createSpriteGeometry(sprite);
							spriteGeometry.mRenderQueue = sprite.mRenderQueue;
							geometries.push_back(&spriteGeometry);
						}
						break;
					}

					case RenderItem::Type::VIEWPORT:
					{
						const renderitems::Viewport& viewport = static_cast<const renderitems::Viewport&>(*renderItem);

						Geometry& geometry = mGeometryFactory.createViewportGeometry(Recti(viewport.mPosition, viewport.mSize));
						geometry.mRenderQueue = viewport.mRenderQueue;
						geometries.push_back(&geometry);
						break;
					}

					default:
						break;
				}
			}
		}
	}

	// Insert blur effect geometry at the right position
	if (Configuration::instance().mBackgroundBlur > 0)
	{
		constexpr uint16 BLUR_RENDER_QUEUE = 0x1800;

		// Anything there to blur at all?
		//  -> There might be no blurred background at all (e.g. in S3K Sky Sanctuary upper levels)
		bool blurNeeded = false;
		for (const Geometry* geometry : geometries)
		{
			if (geometry->mRenderQueue < BLUR_RENDER_QUEUE)
			{
				blurNeeded = true;
				break;
			}
		}

		if (blurNeeded)
		{
			Geometry& geometry = mGeometryFactory.createEffectBlurGeometry(Configuration::instance().mBackgroundBlur);
			geometry.mRenderQueue = BLUR_RENDER_QUEUE - 1;
			geometries.push_back(&geometry);
		}
	}

	// Sort everything by render queue
	std::stable_sort(geometries.begin(), geometries.end(),
					 [](const Geometry* a, const Geometry* b) { return a->mRenderQueue < b->mRenderQueue; });
}

void VideoOut::renderGameScreen()
{
#ifdef RMX_WITH_OPENGL_SUPPORT
	if (Configuration::instance().mStereoEyeSeparation > 0 && mActiveRenderer == mOpenGLRenderer)
	{
		renderGameScreenStereo();
		return;
	}
#endif

	// Normal single-eye render
	clearGeometries();
	if (mRenderParts->getActiveDisplay())
		collectGeometries(mGeometries);
	mActiveRenderer->renderGameScreen(mGeometries);
}

#ifdef RMX_WITH_OPENGL_SUPPORT
void VideoOut::setupStereoBuffers()
{
	if (mStereoBuffersReady)
		return;

	// Create the 2x-wide stereo output texture
	mStereoTexture.setupAsRenderTarget(Vec2i(mGameResolution.x * 2, mGameResolution.y));

	GLuint gameTexHandle  = mGameScreenTexture.getImplementation<OpenGLDrawerTexture>()->getTextureHandle();
	GLuint stereoTexHandle = mStereoTexture.getImplementation<OpenGLDrawerTexture>()->getTextureHandle();

	// Read FBO: reads from the game screen texture (same texture the renderer writes to)
	glGenFramebuffers(1, &mStereoReadFBOHandle);
	glBindFramebuffer(GL_FRAMEBUFFER, mStereoReadFBOHandle);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexHandle, 0);

	// Write FBO: writes to the 2x-wide stereo texture
	glGenFramebuffers(1, &mStereoWriteFBOHandle);
	glBindFramebuffer(GL_FRAMEBUFFER, mStereoWriteFBOHandle);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stereoTexHandle, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	mStereoBuffersReady = true;
}

void VideoOut::renderGameScreenStereo()
{
	setupStereoBuffers();

	ScrollOffsetsManager& scrollMgr = mRenderParts->getScrollOffsetsManager();

	// Save scroll offsets (base + interpolated) so we can restore them after both eyes
	ScrollOffsetsManager::StereoScrollSnapshot savedScrollH;
	scrollMgr.saveScrollOffsetsH(savedScrollH);

	const int sep = Configuration::instance().mStereoEyeSeparation;

	// Per-scanline velocity-based parallax ratio:
	// Compare Plane B (set 0) and Plane A (set 1) scroll deltas frame-to-frame.
	// Where bg moves slower than fg the ratio < 1 → less depth shift for that scanline.
	if (!mPrevStereoScrollValid)
	{
		// First frame: seed ratios to 0.5 so debug pan shows parallax immediately
		for (int k = 0; k < 0x100; ++k)
			mCachedBgRatios[k] = 0.5f;
	}
	else
	{
		// Average fg velocity across all scanlines
		float fgAvg = 0.0f;
		for (int k = 0; k < 0x100; ++k)
			fgAvg += (int16)(savedScrollH.base[1][k] - mPrevStereoScrollH.base[1][k]);
		fgAvg /= 0x100;

		if (fabsf(fgAvg) > 0.5f)
		{
			// Exponential smoothing (alpha=0.15) damps frame-to-frame ratio noise
			// that would otherwise cause ±1px jitter on Plane B when running
			static constexpr float kAlpha = 0.15f;
			for (int k = 0; k < 0x100; ++k)
			{
				const float bgDelta = (float)(int16)(savedScrollH.base[0][k] - mPrevStereoScrollH.base[0][k]);
				const float newRatio = clamp(bgDelta / fgAvg, 0.0f, 1.0f);
				mCachedBgRatios[k] = mCachedBgRatios[k] * (1.0f - kAlpha) + newRatio * kAlpha;
			}
		}
		// If fgAvg ≈ 0 (standing still), keep mCachedBgRatios as-is from last moving frame
	}
	mPrevStereoScrollH   = savedScrollH;
	mPrevStereoScrollValid = true;

	// Pass 0 = right-eye view (camera + sep/2) → left panel
	// Pass 1 = left-eye  view (camera - sep/2) → right panel
	for (int eye = 0; eye < 2; ++eye)
	{
		const int shift  = (eye == 0) ? (sep / 2) : -(sep / 2);
		const int panelX = (eye == 0) ? 0 : mGameResolution.x;

		const int debugOffset = Configuration::instance().mStereoCameraDebugOffset;
		const int totalShift = shift + debugOffset;

		// Plane A (floor, walls, near BG) gets the full stereo shift.
		// Sprites are not touched — they sit flat at screen surface.
		// Plane B is scaled by velocity ratio relative to Plane A so it always appears behind it.
		const int fgShift = totalShift;

		int eyeBgShifts[0x100];
		for (int k = 0; k < 0x100; ++k)
			eyeBgShifts[k] = roundToInt((float)totalShift * mCachedBgRatios[k]);

		// Restore saved offsets then apply per-scanline per-plane eye shifts
		scrollMgr.restoreScrollOffsetsH(savedScrollH);
		scrollMgr.shiftAllScrollOffsetsH(fgShift, eyeBgShifts);

		// Render this eye into mGameScreenTexture.
		clearGeometries();
		if (mRenderParts->getActiveDisplay())
			collectGeometries(mGeometries);

		// Shift every sprite (world-space, screen-space, HUD) by the same camera shift
		// so sprites, Plane A tiles, and HUD all sit at the same visual depth layer.
		SpriteManager& spriteMgr = mRenderParts->getSpriteManager();
		for (int ctx = 0; ctx < RenderItem::NUM_LIFETIME_CONTEXTS; ++ctx)
		{
			for (RenderItem* item : spriteMgr.getRenderItems((RenderItem::LifetimeContext)ctx))
			{
				if (!item->isSprite())
					continue;
				static_cast<renderitems::SpriteInfo*>(item)->mInterpolatedPosition.x -= fgShift;
			}
		}

		mActiveRenderer->renderGameScreen(mGeometries);

		// Blit the rendered eye into the correct half of the stereo texture
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mStereoReadFBOHandle);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mStereoWriteFBOHandle);
		glBlitFramebuffer(
			0,       0, mGameResolution.x,          mGameResolution.y,
			panelX,  0, panelX + mGameResolution.x, mGameResolution.y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST
		);
	}

	// Restore original scroll offsets
	scrollMgr.restoreScrollOffsetsH(savedScrollH);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Debug subtraction view (= key): shift-corrected difference between eyes.
	// A = left panel (right eye) at x; B = right panel (left eye) at x+sep.
	// For player-depth content A≈B → white. A>B → orange (in front). A<B → blue (behind).
	if (mStereoDebugMode)
	{
		const int w = mGameResolution.x;
		const int h = mGameResolution.y;

		mStereoDebugBuffer.resize(w * 2 * h * 4);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mStereoWriteFBOHandle);
		glReadPixels(0, 0, w * 2, h, GL_RGBA, GL_UNSIGNED_BYTE, mStereoDebugBuffer.data());
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

		std::vector<uint8> composite(w * h * 4);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const uint8* A = &mStereoDebugBuffer[(y * w * 2 + x) * 4];
				const float aLum = (A[0] * 0.299f + A[1] * 0.587f + A[2] * 0.114f) / 255.0f;

				float bLum = 0.0f;
				const int bx = x + sep;  // compareOffset = sep: full Plane A shift for both eyes
				if (bx < w)
				{
					const uint8* B = &mStereoDebugBuffer[(y * w * 2 + w + bx) * 4];
					bLum = (B[0] * 0.299f + B[1] * 0.587f + B[2] * 0.114f) / 255.0f;
				}

				const float total = (aLum + bLum) * 0.5f;
				const float diff  = (aLum - bLum) * 6.0f;

				uint8* out = &composite[(y * w + x) * 4];
				out[3] = 0xff;

				if (total < 0.015f)
				{
					out[0] = out[1] = out[2] = 0;
				}
				else if (diff > 0.15f)
				{
					// In front: orange
					const uint8 v = (uint8)(std::min(diff, 1.0f) * 255.0f);
					out[0] = v;
					out[1] = v / 2;
					out[2] = 0;
				}
				else if (diff < -0.15f)
				{
					// Behind: blue
					const uint8 v = (uint8)(std::min(-diff, 1.0f) * 255.0f);
					out[0] = 0;
					out[1] = v / 4;
					out[2] = v;
				}
				else
				{
					// Player depth: white/gray
					const uint8 g = (uint8)(total * 255.0f);
					out[0] = out[1] = out[2] = g;
				}
			}
		}

		OpenGLDrawerTexture* stereoImpl = mStereoTexture.getImplementation<OpenGLDrawerTexture>();
		glBindTexture(GL_TEXTURE_2D, stereoImpl->getTextureHandle());
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, composite.data());
		glTexSubImage2D(GL_TEXTURE_2D, 0, w, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, composite.data());
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}
#endif

void VideoOut::preRefreshDebugging()
{
	mRenderParts->getSpriteManager().clearLifetimeContext(RenderItem::LifetimeContext::OUTSIDE_FRAME);
}

void VideoOut::postRefreshDebugging()
{
	const bool hasNewRenderItems = !mRenderParts->getSpriteManager().getAddedItems().empty();
	mDebugDrawRenderingRequested = hasNewRenderItems || mPreviouslyHadNewRenderItems;
	mPreviouslyHadNewRenderItems = hasNewRenderItems;

	if (hasNewRenderItems)
	{
		mRenderParts->getSpriteManager().postRefreshDebugging();
	}
}

void VideoOut::renderDebugDraw(int debugDrawMode, const Recti& rect)
{
	mActiveRenderer->renderDebugDraw(debugDrawMode, rect);
}

void VideoOut::dumpDebugDraw(int debugDrawMode)
{
	if (debugDrawMode < 2)
	{
		mRenderParts->dumpPlaneContent(debugDrawMode);
	}
	else
	{
		mRenderParts->dumpPatternsContent();
	}
}
