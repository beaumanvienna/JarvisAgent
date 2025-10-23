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
#include <functional>
#include <sstream>

namespace AIAssistant
{
    // -----------------------------
    // EVENT TYPES AND CATEGORIES
    // -----------------------------
    enum class EventType
    {
        None = 0,
        KeyPressed,          //
        KeyReleased,         //
        MouseMoved,          //
        MouseButtonPressed,  //
        MouseButtonReleased, //
        TimerElapsed,        //
        FileAdded,           //
        FileRemoved,         //
        FileModified,        //
        AppError,            //
        EngineEvent          //
    };

    enum EventCategory
    {
        NoneCategory = 0,               //
        EventCategoryKeyboard = 1 << 0, //
        EventCategoryMouse = 1 << 1,    //
        EventCategoryTimer = 1 << 2,    //
        EventCategoryFileSys = 1 << 3,  //
        EventCategoryApp = 1 << 4,      //
        EventCategoryEngine = 1 << 5,   //
    };

    // ---------------------------------
    // BASE EVENT CLASS
    // ---------------------------------
    class Event
    {
    public:
        virtual ~Event() = default;

        bool IsHandled() const { return m_Handled; }
        void SetHandled(bool handled = true) { m_Handled = handled; }

        virtual EventType GetEventType() const = 0;
        virtual const char* GetName() const = 0;
        virtual int GetCategoryFlags() const = 0;

        bool IsInCategory(EventCategory category) const { return GetCategoryFlags() & category; }

        virtual std::string ToString() const { return GetName(); }

    protected:
        bool m_Handled = false;
    };

    // ---------------------------------
    // EVENT DISPATCHER
    // ---------------------------------
    class EventDispatcher
    {
        template <typename T> using EventFn = std::function<bool(T&)>;

    public:
        EventDispatcher(Event& event) : m_Event(event) {}

        template <typename T> bool Dispatch(EventFn<T> func)
        {
            if (m_Event.GetEventType() == T::GetStaticType())
            {
                m_Event.SetHandled(func(static_cast<T&>(m_Event)));
                return true;
            }
            return false;
        }

    private:
        Event& m_Event;
    };

#define EVENT_CLASS_TYPE(type)                                                  \
    static EventType GetStaticType() { return EventType::type; }                \
    virtual EventType GetEventType() const override { return GetStaticType(); } \
    virtual const char* GetName() const override { return #type; }

#define EVENT_CLASS_CATEGORY(category) \
    virtual int GetCategoryFlags() const override { return category; }

} // namespace AIAssistant
