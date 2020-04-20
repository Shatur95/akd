/*
 *  Copyright © 2019-2020 Hennadii Chernyshchyk <genaloner@gmail.com>
 *
 *  This file is part of Advanced Keyboard Daemon.
 *
 *  Crow Translate is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a get of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "keyboarddaemon.h"
#include "keyboardsymbolsparser.h"

#include <iostream>

#include <boost/spirit/home/x3.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/program_options.hpp>

namespace x3 = boost::spirit::x3;
namespace po = boost::program_options;

KeyboardDaemon::KeyboardDaemon(po::variables_map &parameters)
{
    XSelectInput(m_display.get(), m_root, PropertyChangeMask | SubstructureNotifyMask);
    XkbSelectEvents(m_display.get(), XkbUseCoreKbd, XkbIndicatorStateNotifyMask, XkbIndicatorStateNotifyMask);

    m_printGroups = parameters["general.print-groups"].as<bool>();

    const KeyboardSymbols serverSymbols = parseServerSymbols();
    for (const std::string &layout : parameters["general.layouts"].as<std::vector<std::string>>())
        m_layouts.emplace_back(layout, serverSymbols.options);

    if (m_layouts.empty())
        m_layouts.emplace_back(boost::join(serverSymbols.groups, ","));
    else
        setLayout(0);

    auto it = parameters.find("shortcuts.nextlayout");
    if (it != parameters.end())
        m_shortcuts.emplace_back(std::move(it->second.as<std::string &&>()), *this, &KeyboardDaemon::switchToNextLayout);

    saveCurrentGroup();
}

void KeyboardDaemon::processEvents()
{
    while (true) {
        XEvent event;
        XNextEvent(m_display.get(), &event);

        switch (event.type) {
        case DestroyNotify:
            removeDestroyedWindow(event.xdestroywindow);
            break;
        case PropertyNotify:
            applyLayout(event.xproperty);
            break;
        case KeyPress:
            processShortcuts(event.xkey);
            break;
        default:
            if (event.type == m_xkbEventType)
                saveCurrentGroup();
        }
    }
}

Display &KeyboardDaemon::display() const
{
    return *m_display;
}

Window KeyboardDaemon::root() const
{
    return m_root;
}

void KeyboardDaemon::switchToNextLayout()
{
    m_currentWindow->second.layoutIndex += 1;
    if (m_currentWindow->second.layoutIndex >= m_layouts.size())
        m_currentWindow->second.layoutIndex = 0;

    setLayout(m_currentWindow->second.layoutIndex);
}

void KeyboardDaemon::removeDestroyedWindow(const XDestroyWindowEvent &event)
{
    m_windows.erase(event.window);
}

void KeyboardDaemon::processShortcuts(const XKeyEvent &event)
{
    for (const Shortcut &shortcut : m_shortcuts)
        shortcut.processEvent(event);
}

void KeyboardDaemon::applyLayout(const XPropertyEvent &event)
{
    if (event.state != PropertyNewValue || event.atom != m_activeWindowProperty)
        return;

    const auto [newWindow, inserted] = m_windows.try_emplace(activeWindow());
    if (newWindow->second.layoutIndex != m_currentWindow->second.layoutIndex)
        setLayout(newWindow->second.layoutIndex);
    if (newWindow->second.group != m_currentWindow->second.group)
        setGroup(newWindow->second.group);
    m_currentWindow = newWindow;
}

void KeyboardDaemon::saveCurrentGroup()
{
    XkbStateRec state;
    XkbGetState(m_display.get(), XkbUseCoreKbd, &state);

    m_currentWindow->second.group = state.group;

    if (m_printGroups)
        std::cout << m_layouts[m_currentWindow->second.layoutIndex].groupName(m_currentWindow->second.group) << std::endl;
}

void KeyboardDaemon::setLayout(size_t layoutIndex)
{
    // Replace layouts with specified and generate new symbols string
    m_currentComponents.symbols = m_layouts[layoutIndex].symbols.data();

    // Send it back to X11
    const std::unique_ptr<XkbDescRec, XlibDeleter> newDesc(XkbGetKeyboardByName(m_display.get(), XkbUseCoreKbd, &m_currentComponents, XkbGBN_ClientSymbolsMask | XkbGBN_KeyNamesMask, 0, true));
    if (!newDesc)
        throw std::logic_error("Unable to build keyboard description with the following symbols: " + m_layouts[layoutIndex].symbols);
}

void KeyboardDaemon::setGroup(unsigned char group)
{
    if (!XkbLockGroup(m_display.get(), XkbUseCoreKbd, group))
        throw std::logic_error("Unable to switch group to " + std::to_string(group));
}

KeyboardSymbols KeyboardDaemon::parseServerSymbols()
{
    const std::unique_ptr<XkbDescRec, XlibDeleter> currentDesc(XkbGetKeyboardByName(m_display.get(), XkbUseCoreKbd, nullptr, XkbGBN_ServerSymbolsMask | XkbGBN_KeyNamesMask, 0, false));
    if (!currentDesc)
        throw std::logic_error("Unable to read keyboard symbols");

    KeyboardSymbols symbols;
    const std::unique_ptr<char [], XlibDeleter> currentSymbols(XGetAtomName(m_display.get(), currentDesc->names->symbols));
    x3::parse(currentSymbols.get(), currentSymbols.get() + strlen(currentSymbols.get()), KeyboardSymbolsParser::symbolsRule, symbols);

    return symbols;
}

Window KeyboardDaemon::activeWindow()
{
    Atom type;
    int format;
    unsigned long size;
    unsigned long remainSize;
    unsigned char *bytes;

    XGetWindowProperty(m_display.get(), m_root, m_activeWindowProperty, 0, 1, false, AnyPropertyType,
                       &type, &format, &size, &remainSize, &bytes);

    std::unique_ptr<unsigned char [], XlibDeleter> cleaner(bytes);

    return *reinterpret_cast<Window *>(bytes);
}
