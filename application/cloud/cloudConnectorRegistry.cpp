/* Copyright (c) 2026 JC Technolabs
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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "engine.h"
#include "cloud/cloudConnectorRegistry.h"

namespace AIAssistant
{
    void CloudConnectorRegistry::Register(std::unique_ptr<ICloudConnector> connector)
    {
        if (!connector)
        {
            LOG_CORE_WARN("CloudConnectorRegistry::Register: null connector");
            return;
        }

        std::string type = connector->GetType();
        if (m_Connectors.count(type))
        {
            LOG_CORE_WARN("CloudConnectorRegistry::Register: connector '{}' already registered, replacing", type);
        }

        LOG_CORE_INFO("CloudConnectorRegistry::Register: registered connector '{}'", type);
        m_Connectors[type] = std::move(connector);
    }

    ICloudConnector* CloudConnectorRegistry::GetConnector(std::string const& type) const
    {
        auto const it = m_Connectors.find(type);
        if (it == m_Connectors.end())
        {
            return nullptr;
        }
        return it->second.get();
    }

    std::vector<std::string> CloudConnectorRegistry::GetRegisteredTypes() const
    {
        std::vector<std::string> types;
        types.reserve(m_Connectors.size());
        for (auto const& [type, connector] : m_Connectors)
        {
            types.push_back(type);
        }
        return types;
    }
} // namespace AIAssistant
