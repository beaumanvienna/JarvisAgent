/* Copyright (c) 2025 JC Technolabs
   License: GPL-3.0

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
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.*/

#pragma once

#include "task/taskBase.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AIAssistant
{
    class IInternalTaskRegistry
    {
    public:
        virtual ~IInternalTaskRegistry() = default;
        virtual std::unique_ptr<ITask> CreateTask(std::string const& action) const = 0;
    };

    class InternalTaskRegistry final : public IInternalTaskRegistry
    {
    public:
        using TaskFactory = std::function<std::unique_ptr<ITask>()>;

    public:
        InternalTaskRegistry() = default;
        virtual ~InternalTaskRegistry() = default;

        void RegisterFactory(std::string const& action, TaskFactory const& factory)
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            m_Factories[action] = factory;
        }

        std::unique_ptr<ITask> CreateTask(std::string const& action) const override
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            auto iterator = m_Factories.find(action);
            if (iterator == m_Factories.end())
            {
                return nullptr;
            }

            return iterator->second();
        }

    private:
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, TaskFactory> m_Factories;
    };

} // namespace AIAssistant
