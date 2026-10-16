# Project headers deliberately stay outside the PCH, keeping feature-dependent
# class layouts and fixture seams under each target's existing definitions.
function(okuflow_enable_pch target)
    if(NOT OKUFLOW_USE_PCH)
        return()
    endif()
    set(headers <algorithm> <memory> <string> <vector>)
    get_target_property(libraries ${target} LINK_LIBRARIES)
    if("Qt6::Core" IN_LIST libraries OR "Qt6::Widgets" IN_LIST libraries)
        list(APPEND headers <QtCore/QObject> <QtCore/QString> <QtCore/QJsonObject>
            <QtCore/QJsonDocument> <QtCore/QCoreApplication>)
    endif()
    if("Qt6::Gui" IN_LIST libraries OR "Qt6::Widgets" IN_LIST libraries)
        list(APPEND headers <QtGui/QColor> <QtGui/QImage>)
    endif()
    if("Qt6::Widgets" IN_LIST libraries)
        list(APPEND headers <QtWidgets/QWidget> <QtWidgets/QApplication>)
    endif()
    get_target_property(definitions ${target} COMPILE_DEFINITIONS)
    if(WIN32 AND "NOMINMAX" IN_LIST definitions AND "WIN32_LEAN_AND_MEAN" IN_LIST definitions)
        # winsock2 must precede Windows.h for capture/network targets.
        list(APPEND headers <winsock2.h> <windows.h>)
    endif()
    foreach(header IN LISTS headers)
        # nvcc retains its existing include order and flags in mixed targets.
        string(REPLACE ">" "$<ANGLE-R>" header "${header}")
        target_precompile_headers(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${header}>")
    endforeach()
endfunction()
