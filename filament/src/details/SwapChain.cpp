/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "details/SwapChain.h"

#include "details/Engine.h"

#include <utils/Systrace.h>

namespace filament {

FSwapChain::FSwapChain(FEngine& engine, void* nativeWindow, uint64_t flags)
        : mEngine(engine), mNativeWindow(nativeWindow), mConfigFlags(flags) {
    SYSTRACE_CALL();
    mSwapChain = engine.getDriverApi().createSwapChain(nativeWindow, flags);
}

FSwapChain::FSwapChain(FEngine& engine, uint32_t width, uint32_t height, uint64_t flags)
        : mEngine(engine), mConfigFlags(flags) {
    SYSTRACE_CALL();
    mSwapChain = engine.getDriverApi().createSwapChainHeadless(width, height, flags);
}

void FSwapChain::terminate(FEngine& engine) noexcept {
    SYSTRACE_CALL();
    engine.getDriverApi().destroySwapChain(mSwapChain);
}

void FSwapChain::setFrameScheduledCallback(FrameScheduledCallback callback, void* user) {
    SYSTRACE_CALL();
    mEngine.getDriverApi().setFrameScheduledCallback(mSwapChain, callback, user);
}

void FSwapChain::setFrameCompletedCallback(backend::CallbackHandler* handler,
                utils::Invocable<void(SwapChain*)>&& callback) noexcept {
    SYSTRACE_CALL();
    struct Callback {
        utils::Invocable<void(SwapChain*)> f;
        SwapChain* s;
        static void func(void* user) {
            auto* const c = reinterpret_cast<Callback*>(user);
            c->f(c->s);
            delete c;
        }
    };
    if (callback) {
        auto* const user = new(std::nothrow) Callback{ std::move(callback), this };
        mEngine.getDriverApi().setFrameCompletedCallback(
                mSwapChain, handler, &Callback::func, static_cast<void*>(user));
    } else {
        mEngine.getDriverApi().setFrameCompletedCallback(mSwapChain, nullptr, nullptr, nullptr);
    }
}

bool FSwapChain::isSRGBSwapChainSupported(FEngine& engine) noexcept {
    SYSTRACE_CALL();
    return engine.getDriverApi().isSRGBSwapChainSupported();
}

} // namespace filament
