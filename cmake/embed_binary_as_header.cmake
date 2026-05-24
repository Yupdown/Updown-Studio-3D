if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is not defined")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is not defined")
endif()

if(NOT DEFINED SYMBOL)
    message(FATAL_ERROR "SYMBOL is not defined")
endif()

file(READ "${INPUT}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

set(HEADER_TEXT "#pragma once\n\nnamespace udsdx\n{\n")
string(APPEND HEADER_TEXT "inline constexpr unsigned char ${SYMBOL}[] = {\n")

if(BYTE_COUNT GREATER 0)
    math(EXPR LAST_INDEX "${BYTE_COUNT} - 1")
    foreach(i RANGE 0 ${LAST_INDEX})
        math(EXPR HEX_OFFSET "${i} * 2")
        string(SUBSTRING "${HEX_DATA}" ${HEX_OFFSET} 2 BYTE_HEX)
        string(APPEND HEADER_TEXT "0x${BYTE_HEX}, ")

        math(EXPR COLUMN "${i} % 12")
        if(COLUMN EQUAL 11)
            string(APPEND HEADER_TEXT "\n")
        endif()
    endforeach()
endif()

string(APPEND HEADER_TEXT "0x00\n};\n")
string(APPEND HEADER_TEXT "inline constexpr unsigned int ${SYMBOL}_size = ${BYTE_COUNT};\n")
string(APPEND HEADER_TEXT "}\n")

file(WRITE "${OUTPUT}" "${HEADER_TEXT}")
