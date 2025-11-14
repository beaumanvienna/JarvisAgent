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

#include <string>

// Forward declaration to avoid including Python headers here
struct _object;
typedef _object PyObject;

namespace AIAssistant
{
    class Event;

    class PythonEngine
    {
    public:
        PythonEngine();
        ~PythonEngine();

        bool Initialize(std::string const& scriptPath);
        void Shutdown();

        void OnStart();
        void OnUpdate();
        void OnEvent(Event const& event);
        void OnShutdown();

        bool IsRunning() const { return m_Running; }

    private:
        void Reset();
        bool LoadModule();
        void LoadHooks();

        void CallHook(PyObject* function, char const* hookName);
        void CallHookWithEvent(PyObject* function, char const* hookName, Event const& event);

        PyObject* BuildEventDict(Event const& event);

    private:
        bool m_Running{false};

        std::string m_ScriptPath;
        std::string m_ScriptDir;
        std::string m_ModuleName;

        PyObject* m_MainModule{nullptr};
        PyObject* m_MainDict{nullptr};

        PyObject* m_OnStartFunc{nullptr};
        PyObject* m_OnUpdateFunc{nullptr};
        PyObject* m_OnEventFunc{nullptr};
        PyObject* m_OnShutdownFunc{nullptr};
    };

} // namespace AIAssistant
