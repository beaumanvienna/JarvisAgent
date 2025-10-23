/* Copyright (c) 2025 JC Technolabs

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <memory>

#include "application.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
    class JarvisAgent : public Application
    {
    public:
        JarvisAgent() = default;
        ~JarvisAgent() = default;

        virtual void OnStart() override;
        virtual void OnUpdate() override;
        virtual void OnEvent(Event&) override;
        virtual void OnShutdown() override;

        virtual bool IsFinished() override;
        static std::unique_ptr<Application> Create();

    private:
        void CheckIfFinished();

    private:
        [[maybe_unused]] bool m_IsFinished{false};
        CurlWrapper m_Curl;
        std::string m_Url;
        std::string m_Model;
    };
} // namespace AIAssistant
