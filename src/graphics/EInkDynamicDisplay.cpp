#include "configuration.h"

#if defined(USE_EINK) && defined(USE_EINK_DYNAMICDISPLAY)
#include "EInkDynamicDisplay.h"

// Constructor
EInkDynamicDisplay::EInkDynamicDisplay(uint8_t address, int sda, int scl, OLEDDISPLAY_GEOMETRY geometry, HW_I2C i2cBus)
    : EInkDisplay(address, sda, scl, geometry, i2cBus)
{
    // If tracking ghost pixels, grab memory
#ifdef EINK_LIMIT_GHOSTING_PX
    dirtyPixels = new uint8_t[EInkDisplay::displayBufferSize](); // Init with zeros
#endif
}

// Destructor
EInkDynamicDisplay::~EInkDynamicDisplay()
{
    // If we were tracking ghost pixels, free the memory
#ifdef EINK_LIMIT_GHOSTING_PX
    delete[] dirtyPixels;
#endif
}

// Screen requests a BACKGROUND frame
void EInkDynamicDisplay::display()
{
    addFrameFlag(BACKGROUND);
    update();
}

// Screen requests a RESPONSIVE frame
bool EInkDynamicDisplay::forceDisplay(uint32_t msecLimit)
{
    addFrameFlag(RESPONSIVE);
    return update(); // (Unutilized) Base class promises to return true if update ran
}

// Add flag for the next frame
void EInkDynamicDisplay::addFrameFlag(frameFlagTypes flag)
{
    // OR the new flag into the existing flags
    this->frameFlags = (frameFlagTypes)(this->frameFlags | flag);
}

// GxEPD2 code to set fast refresh
void EInkDynamicDisplay::configForFastRefresh()
{
    // Variant-specific code can go here
#if defined(PRIVATE_HW)
#else
    // Otherwise:
    adafruitDisplay->setPartialWindow(0, 0, adafruitDisplay->width(), adafruitDisplay->height());
#endif
}

// GxEPD2 code to set full refresh
void EInkDynamicDisplay::configForFullRefresh()
{
    // Variant-specific code can go here
#if defined(PRIVATE_HW)
#else
    // Otherwise:
    adafruitDisplay->setFullWindow();
#endif
}

// Run any relevant GxEPD2 code, so next update will use correct refresh type
void EInkDynamicDisplay::applyRefreshMode()
{
    // Change from FULL to FAST
    if (currentConfig == FULL && refresh == FAST) {
        configForFastRefresh();
        currentConfig = FAST;
    }

    // Change from FAST back to FULL
    else if (currentConfig == FAST && refresh == FULL) {
        configForFullRefresh();
        currentConfig = FULL;
    }
}

// Update fastRefreshCount
void EInkDynamicDisplay::adjustRefreshCounters()
{
    if (refresh == FAST)
        fastRefreshCount++;

    else if (refresh == FULL)
        fastRefreshCount = 0;
}

// Trigger the display update by calling base class
bool EInkDynamicDisplay::update()
{
    // Detemine the refresh mode to use, and start the update
    bool refreshApproved = determineMode();
    if (refreshApproved)
        EInkDisplay::forceDisplay(0); // Bypass base class' own rate-limiting system

#if defined(HAS_EINK_ASYNCFULL)
    if (refreshApproved)
        endOrDetach(); // Either endUpdate() right now (fast refresh), or set the async flag (full refresh)
#endif

    storeAndReset();        // Store the result of this loop for next time
    return refreshApproved; // (Unutilized) Base class promises to return true if update ran
}

// Assess situation, pick a refresh type
bool EInkDynamicDisplay::determineMode()
{
    checkForPromotion();
#if defined(HAS_EINK_ASYNCFULL)
    checkAsyncFullRefresh();
#endif
    checkRateLimiting();

    // If too soon for a new frame, or display busy, abort early
    if (refresh == SKIPPED)
        return false; // No refresh

    // -- New frame is due --

    resetRateLimiting(); // Once determineMode() ends, will have to wait again
    hashImage();         // Generate here, so we can still copy it to previousImageHash, even if we skip the comparison check
    LOG_DEBUG("determineMode(): "); // Begin log entry

    // Once mode determined, any remaining checks will bypass
    checkCosmetic();
    checkDemandingFast();
    checkConsecutiveFastRefreshes();
#ifdef EINK_LIMIT_GHOSTING_PX
    checkExcessiveGhosting();
#endif
    checkFrameMatchesPrevious();
    checkFastRequested();

    if (refresh == UNSPECIFIED)
        LOG_WARN("There was a flaw in the determineMode() logic.\n");

    // -- Decision has been reached --
    applyRefreshMode();
    adjustRefreshCounters();

#ifdef EINK_LIMIT_GHOSTING_PX
    // Full refresh clears any ghosting
    if (refresh == FULL)
        resetGhostPixelTracking();
#endif

    // Return - call a refresh or not?
    if (refresh == SKIPPED)
        return false; // Don't trigger a refresh
    else
        return true; // Do trigger a refresh
}

    }
}

// Was a frame skipped (rate, display busy) that should have been a FAST refresh?
void EInkDynamicDisplay::checkForPromotion()
{
    // If a frame was skipped (rate, display busy), then promote a BACKGROUND frame
    // Because we DID want a RESPONSIVE/COSMETIC/DEMAND_FULL frame last time, we just didn't get it

    switch (previousReason) {
    case ASYNC_REFRESH_BLOCKED_DEMANDFAST:
        addFrameFlag(DEMAND_FAST);
        break;
    case ASYNC_REFRESH_BLOCKED_COSMETIC:
        addFrameFlag(COSMETIC);
        break;
    case ASYNC_REFRESH_BLOCKED_RESPONSIVE:
    case EXCEEDED_RATELIMIT_FAST:
        addFrameFlag(RESPONSIVE);
        break;
    default:
        break;
    }
}

// Is it too soon for another frame of this type?
void EInkDynamicDisplay::checkRateLimiting()
{
    uint32_t now = millis();

    // Sanity check: millis() overflow - just let the update run..
    if (previousRunMs > now)
        return;

    // Skip update: too soon for BACKGROUND
    if (frameFlags == BACKGROUND) {
        if (now - previousRunMs < EINK_LIMIT_RATE_BACKGROUND_SEC * 1000) {
            refresh = SKIPPED;
            reason = EXCEEDED_RATELIMIT_FULL;
            return;
        }
    }

    // No rate-limit for these special cases
    if (frameFlags & COSMETIC || frameFlags & DEMAND_FAST)
        return;

    // Skip update: too soon for RESPONSIVE
    if (frameFlags & RESPONSIVE) {
        if (now - previousRunMs < EINK_LIMIT_RATE_RESPONSIVE_SEC * 1000) {
            refresh = SKIPPED;
            reason = EXCEEDED_RATELIMIT_FAST;
            return;
        }
    }
}

// Is this frame COSMETIC (splash screens?)
void EInkDynamicDisplay::checkCosmetic()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    // A full refresh is requested for cosmetic purposes: we have a decision
    if (frameFlags & COSMETIC) {
        refresh = FULL;
        reason = FLAGGED_COSMETIC;
        LOG_DEBUG("refresh=FULL, reason=FLAGGED_COSMETIC\n");
    }
}

// Is this a one-off special circumstance, where we REALLY want a fast refresh?
void EInkDynamicDisplay::checkDemandingFast()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    // A fast refresh is demanded: we have a decision
    if (frameFlags & DEMAND_FAST) {
        refresh = FAST;
        reason = FLAGGED_DEMAND_FAST;
        LOG_DEBUG("refresh=FAST, reason=FLAGGED_DEMAND_FAST\n");
    }
}

// Have too many fast-refreshes occured consecutively, since last full refresh?
void EInkDynamicDisplay::checkConsecutiveFastRefreshes()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    // If too many FAST refreshes consecutively - force a FULL refresh
    if (fastRefreshCount >= EINK_LIMIT_FASTREFRESH) {
        refresh = FULL;
        reason = EXCEEDED_LIMIT_FASTREFRESH;
        LOG_DEBUG("refresh=FULL, reason=EXCEEDED_LIMIT_FASTREFRESH\n");
    }
}

// Does the new frame match the currently displayed image?
void EInkDynamicDisplay::checkFrameMatchesPrevious()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    // If frame is *not* a duplicate, abort the check
    if (imageHash != previousImageHash)
        return;

#if !defined(EINK_BACKGROUND_USES_FAST)
    // If BACKGROUND, and last update was FAST: redraw the same image in FULL (for display health + image quality)
    if (frameFlags == BACKGROUND && fastRefreshCount > 0) {
        refresh = FULL;
        reason = REDRAW_WITH_FULL;
        LOG_DEBUG("refresh=FULL, reason=REDRAW_WITH_FULL\n");
        return;
    }
#endif

    // Not redrawn, not COSMETIC, not DEMAND_FAST
    refresh = SKIPPED;
    reason = FRAME_MATCHED_PREVIOUS;
    LOG_DEBUG("refresh=SKIPPED, reason=FRAME_MATCHED_PREVIOUS\n");
}

// No objections, we can perform fast-refresh, if desired
void EInkDynamicDisplay::checkFastRequested()
{
    if (refresh != UNSPECIFIED)
        return;

    if (frameFlags == BACKGROUND) {
#ifdef EINK_BACKGROUND_USES_FAST
        // If we want BACKGROUND to use fast. (FULL only when a limit is hit)
        refresh = FAST;
        reason = BACKGROUND_USES_FAST;
        LOG_DEBUG("refresh=FAST, reason=BACKGROUND_USES_FAST, fastRefreshCount=%lu\n", fastRefreshCount);
#else
        // If we do want to use FULL for BACKGROUND updates
        refresh = FULL;
        reason = FLAGGED_BACKGROUND;
        LOG_DEBUG("refresh=FULL, reason=FLAGGED_BACKGROUND\n");
#endif
    }

    // Sanity: confirm that we did ask for a RESPONSIVE frame.
    if (frameFlags & RESPONSIVE) {
        refresh = FAST;
        reason = NO_OBJECTIONS;
        LOG_DEBUG("refresh=FAST, reason=NO_OBJECTIONS, fastRefreshCount=%lu\n", fastRefreshCount);
    }
}

// Reset the timer used for rate-limiting
void EInkDynamicDisplay::resetRateLimiting()
{
    previousRunMs = millis();
}

// Generate a hash of this frame, to compare against previous update
void EInkDynamicDisplay::hashImage()
{
    imageHash = 0;

    // Sum all bytes of the image buffer together
    for (uint16_t b = 0; b < (displayWidth / 8) * displayHeight; b++) {
        imageHash += buffer[b];
    }
}

// Store the results of determineMode() for future use, and reset for next call
void EInkDynamicDisplay::storeAndReset()
{
    previousFrameFlags = frameFlags;
    previousRefresh = refresh;
    previousReason = reason;

    // Only store image hash if the display will update
    if (refresh != SKIPPED) {
        previousImageHash = imageHash;
    }

    frameFlags = BACKGROUND;
    refresh = UNSPECIFIED;
}

#ifdef EINK_LIMIT_GHOSTING_PX
// Count how many ghost pixels the new image will display
void EInkDynamicDisplay::countGhostPixels()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    // Start a new count
    ghostPixelCount = 0;

    // Check new image, bit by bit, for any white pixels at locations marked "dirty"
    for (uint16_t i = 0; i < displayBufferSize; i++) {
        for (uint8_t bit = 0; bit < 7; bit++) {

            const bool dirty = (dirtyPixels[i] >> bit) & 1;       // Has pixel location been drawn to since full-refresh?
            const bool shouldBeBlank = !((buffer[i] >> bit) & 1); // Is pixel location white in the new image?

            // If pixel is (or has been) black since last full-refresh, and now is white: ghosting
            if (dirty && shouldBeBlank)
                ghostPixelCount++;

            // Update the dirty status for this pixel - will this location become a ghost if set white in future?
            if (!dirty && !shouldBeBlank)
                dirtyPixels[i] |= (1 << bit);
        }
    }

    LOG_DEBUG("ghostPixels=%hu, ", ghostPixelCount);
}

// Check if ghost pixel count exceeds the defined limit
void EInkDynamicDisplay::checkExcessiveGhosting()
{
    // If a decision was already reached, don't run the check
    if (refresh != UNSPECIFIED)
        return;

    countGhostPixels();

    // If too many ghost pixels, select full refresh
    if (ghostPixelCount > EINK_LIMIT_GHOSTING_PX) {
        refresh = FULL;
        reason = EXCEEDED_GHOSTINGLIMIT;
        LOG_DEBUG("refresh=FULL, reason=EXCEEDED_GHOSTINGLIMIT\n");
    }
}

// Clear the dirty pixels array. Call when full-refresh cleans the display.
void EInkDynamicDisplay::resetGhostPixelTracking()
{
    // Copy the current frame into dirtyPixels[] from the display buffer
    memcpy(dirtyPixels, EInkDisplay::buffer, EInkDisplay::displayBufferSize);
}
#endif // EINK_LIMIT_GHOSTING_PX

#ifdef HAS_EINK_ASYNCFULL
// Check the status of an "async full-refresh", and run the finish-up code if the hardware is ready
void EInkDynamicDisplay::checkAsyncFullRefresh()
{
    // No refresh taking place, continue with determineMode()
    if (!asyncRefreshRunning)
        return;

    // Full refresh still running
    if (adafruitDisplay->epd2.isBusy()) {
        // No refresh
        refresh = SKIPPED;

        // Set the reason, marking what type of frame we're skipping
        if (frameFlags & DEMAND_FAST)
            reason = ASYNC_REFRESH_BLOCKED_DEMANDFAST;
        else if (frameFlags & COSMETIC)
            reason = ASYNC_REFRESH_BLOCKED_COSMETIC;
        else if (frameFlags & RESPONSIVE)
            reason = ASYNC_REFRESH_BLOCKED_RESPONSIVE;
        else
            reason = ASYNC_REFRESH_BLOCKED_BACKGROUND;

        return;
    }

    // If we asyncRefreshRunning flag is still set, but display's BUSY pin reports the refresh is done
    adafruitDisplay->endAsyncFull(); // Run the end of nextPage() code
    EInkDisplay::endUpdate();        // Run base-class code to finish off update (NOT our derived class override)
    asyncRefreshRunning = false;     // Unset the flag
    LOG_DEBUG("Async full-refresh complete\n");

    // Note: this code only works because of a modification to meshtastic/GxEPD2.
    // It is only equipped to intercept calls to nextPage()
}

// Figure out who runs the post-update code
void EInkDynamicDisplay::endOrDetach()
{
    if (previousRefresh == FULL) {  // Note: previousRefresh is the refresh from this loop.
        asyncRefreshRunning = true; // Set the flag - picked up at start of determineMode(), next loop.
        LOG_DEBUG("Async full-refresh begins\n");
    }

    // Fast Refresh
    else
        EInkDisplay::endUpdate(); // Still block while updating, but EInkDisplay needs us to call endUpdate() ourselves.
}
#endif // HAS_EINK_ASYNCFULL

#endif // USE_EINK_DYNAMICDISPLAY