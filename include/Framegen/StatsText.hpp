#pragma once
#include <cstddef>
#include <cstdint>

namespace cvr::framegen::stats_text {
// Invalid sequences consume their first byte and render the existing blank
// fallback. Continuations are examined only up to NUL, so truncated input
// cannot read beyond a NUL-terminated label or leave the caller stuck.
inline uint32_t NextCodepoint(const char*& text) {
    const auto first=static_cast<uint8_t>(*text);
    if(!first)return 0;
    ++text;
    if(first<0x80)return first;
    unsigned count=0;uint32_t value=0,minimum=0;
    if(first>=0xC2 && first<=0xDF) {count=1;value=first&0x1F;minimum=0x80;}
    else if(first>=0xE0 && first<=0xEF) {count=2;value=first&0x0F;minimum=0x800;}
    else if(first>=0xF0 && first<=0xF4) {count=3;value=first&0x07;minimum=0x10000;}
    else return 0xFFFD;
    const char* next=text;
    for(unsigned i=0;i<count;++i) {
        const auto byte=static_cast<uint8_t>(*next);
        if((byte&0xC0)!=0x80)return 0xFFFD;
        value=(value<<6)|(byte&0x3F);++next;
    }
    if(value<minimum || value>0x10FFFF || (value>=0xD800 && value<=0xDFFF))return 0xFFFD;
    text=next;return value;
}
inline size_t CodepointCount(const char* text) {
    size_t count=0;
    while(*text) {NextCodepoint(text);++count;}
    return count;
}

// Five columns, seven rows; all text is a bounded diagnostic alphabet.
inline const uint8_t* Glyph(uint32_t c) {
    static constexpr uint8_t digits[][5]={{62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},{24,20,18,127,16},
        {39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30}};
    static constexpr uint8_t letters[][5]={{126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},{0,65,127,65,0},{32,64,65,63,1},
        {127,8,20,34,65},{127,64,64,64,64},{127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},{63,64,64,64,63},{31,32,64,32,31},
        {63,64,56,64,63},{99,20,8,20,99},{7,8,112,8,7},{97,81,73,69,67}};
    static constexpr uint8_t lowercase[][5]={{32,84,84,84,120},{127,72,68,68,56},{56,68,68,68,32},{56,68,68,72,127},
        {56,84,84,84,24},{8,126,9,1,2},{12,82,82,82,62},{127,8,4,4,120},{0,68,125,64,0},{32,64,68,61,0},
        {127,16,40,68,0},{0,65,127,64,0},{124,4,24,4,120},{124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},
        {8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,32},{4,63,68,64,32},{60,64,64,32,124},{28,32,64,32,28},
        {60,64,48,64,60},{68,40,16,40,68},{12,80,80,80,60},{68,100,84,76,68}};
    static constexpr uint8_t blank[]={0,0,0,0,0},dot[]={0,96,96,0,0},dash[]={8,8,8,8,8};
    static constexpr uint8_t percent[]={99,19,8,100,99},colon[]={0,54,54,0,0},slash[]={32,16,8,4,2};
    static constexpr uint8_t plus[]={8,8,62,8,8},leftParen[]={0,28,34,65,0},rightParen[]={0,65,34,28,0};
    // Cyrillic labels use the same compact uppercase diagnostic face.
    static constexpr uint8_t cyrillic[][5]={
        {0x7E,0x09,0x09,0x09,0x7E}, // U+0410 А
        {0x7F,0x49,0x49,0x49,0x31}, // U+0411 Б
        {0x7F,0x49,0x49,0x49,0x36}, // U+0412 В
        {0x7F,0x01,0x01,0x01,0x01}, // U+0413 Г
        {0x60,0x3F,0x21,0x3F,0x60}, // U+0414 Д
        {0x7F,0x49,0x49,0x49,0x41}, // U+0415 Е
        {0x63,0x14,0x7F,0x14,0x63}, // U+0416 Ж
        {0x41,0x49,0x49,0x49,0x36}, // U+0417 З
        {0x7F,0x10,0x08,0x04,0x7F}, // U+0418 И
        {0x7C,0x21,0x12,0x09,0x7C}, // U+0419 Й
        {0x7F,0x08,0x14,0x22,0x41}, // U+041A К
        {0x40,0x3E,0x01,0x01,0x7F}, // U+041B Л
        {0x7F,0x02,0x0C,0x02,0x7F}, // U+041C М
        {0x7F,0x08,0x08,0x08,0x7F}, // U+041D Н
        {0x3E,0x41,0x41,0x41,0x3E}, // U+041E О
        {0x7F,0x01,0x01,0x01,0x7F}, // U+041F П
        {0x7F,0x09,0x09,0x09,0x06}, // U+0420 Р
        {0x3E,0x41,0x41,0x41,0x22}, // U+0421 С
        {0x01,0x01,0x7F,0x01,0x01}, // U+0422 Т
        {0x47,0x48,0x48,0x48,0x3F}, // U+0423 У
        {0x1C,0x22,0x7F,0x22,0x1C}, // U+0424 Ф
        {0x63,0x14,0x08,0x14,0x63}, // U+0425 Х
        {0x3F,0x20,0x20,0x7F,0x40}, // U+0426 Ц
        {0x07,0x08,0x08,0x08,0x7F}, // U+0427 Ч
        {0x7F,0x40,0x7F,0x40,0x7F}, // U+0428 Ш
        {0x3F,0x20,0x3F,0x20,0x7F}, // U+0429 Щ
        {0x01,0x7F,0x48,0x48,0x30}, // U+042A Ъ
        {0x7F,0x48,0x78,0x00,0x7F}, // U+042B Ы
        {0x7F,0x48,0x48,0x48,0x30}, // U+042C Ь
        {0x41,0x49,0x49,0x49,0x3E}, // U+042D Э
        {0x7F,0x08,0x3E,0x41,0x3E}, // U+042E Ю
        {0x46,0x29,0x19,0x09,0x7F} // U+042F Я
    };
    static constexpr uint8_t yo[]={0x7C,0x55,0x54,0x55,0x44};
    if(c>='0' && c<='9')return digits[c-'0'];if(c>='A' && c<='Z')return letters[c-'A'];
    if(c>='a' && c<='z')return lowercase[c-'a'];
    if(c>=0x0430 && c<=0x044F)c-=0x20;
    if(c==0x0401 || c==0x0451)return yo;
    if(c>=0x0410 && c<=0x042F)return cyrillic[c-0x0410];
    if(c=='+')return plus;if(c=='(')return leftParen;if(c==')')return rightParen;
    if(c=='%')return percent;if(c==':')return colon;if(c=='/')return slash;
    return c=='.' ? dot : (c=='-' ? dash : blank);
}
} // namespace cvr::framegen::stats_text
