#include "intent.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NORMALIZED_TEXT_CAPACITY 384

static bool contains(
    const char *text,
    const char *needle
)
{
    return (
        text
        && needle
        && needle[0]
        && strstr(text, needle) != NULL
    );
}

static bool contains_any(
    const char *text,
    const char *const *forms
)
{
    if (!text || !forms) return false;

    for (
        size_t index = 0;
        forms[index] != NULL;
        ++index
    ) {
        if (contains(text, forms[index])) {
            return true;
        }
    }

    return false;
}

static bool utf8_decode_one(
    const unsigned char *source,
    uint32_t *codepoint,
    size_t *consumed
)
{
    if (!source || !codepoint || !consumed) {
        return false;
    }

    unsigned char first = source[0];

    if (first < 0x80) {
        *codepoint = first;
        *consumed = 1;
        return true;
    }

    if (
        (first & 0xE0) == 0xC0
        && (source[1] & 0xC0) == 0x80
    ) {
        *codepoint =
            ((uint32_t)(first & 0x1F) << 6)
            | (uint32_t)(source[1] & 0x3F);
        *consumed = 2;
        return true;
    }

    if (
        (first & 0xF0) == 0xE0
        && (source[1] & 0xC0) == 0x80
        && (source[2] & 0xC0) == 0x80
    ) {
        *codepoint =
            ((uint32_t)(first & 0x0F) << 12)
            | ((uint32_t)(source[1] & 0x3F) << 6)
            | (uint32_t)(source[2] & 0x3F);
        *consumed = 3;
        return true;
    }

    *codepoint = first;
    *consumed = 1;
    return false;
}

static size_t utf8_encode_one(
    uint32_t codepoint,
    char *destination,
    size_t capacity
)
{
    if (!destination || capacity == 0) return 0;

    if (codepoint <= 0x7F) {
        destination[0] = (char)codepoint;
        return 1;
    }

    if (
        codepoint <= 0x7FF
        && capacity >= 2
    ) {
        destination[0] =
            (char)(0xC0 | (codepoint >> 6));
        destination[1] =
            (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }

    if (
        codepoint <= 0xFFFF
        && capacity >= 3
    ) {
        destination[0] =
            (char)(0xE0 | (codepoint >> 12));
        destination[1] =
            (char)(
                0x80
                | ((codepoint >> 6) & 0x3F)
            );
        destination[2] =
            (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    return 0;
}

static bool ignored_codepoint(uint32_t cp)
{
    switch (cp) {
        case 0x0009:
        case 0x000A:
        case 0x000D:
        case 0x0020:
        case 0x0021:
        case 0x0022:
        case 0x0027:
        case 0x002C:
        case 0x002E:
        case 0x002F:
        case 0x003A:
        case 0x003B:
        case 0x003F:
        case 0x005C:
        case 0x005F:
        case 0x3000:
        case 0x3001:
        case 0x3002:
        case 0x2018:
        case 0x2019:
        case 0x201C:
        case 0x201D:
        case 0xFF01:
        case 0xFF0C:
        case 0xFF0E:
        case 0xFF1A:
        case 0xFF1B:
        case 0xFF1F:
            return true;
        default:
            return false;
    }
}

static void normalize_text(
    const char *source,
    char *destination,
    size_t capacity
)
{
    if (!destination || capacity == 0) return;
    destination[0] = '\0';
    if (!source) return;

    const unsigned char *bytes =
        (const unsigned char *)source;
    size_t read_index = 0;
    size_t write_index = 0;

    while (
        bytes[read_index]
        && write_index + 1 < capacity
    ) {
        uint32_t cp = 0;
        size_t consumed = 1;

        utf8_decode_one(
            bytes + read_index,
            &cp,
            &consumed
        );
        read_index += consumed;

        if (ignored_codepoint(cp)) continue;

        if (
            cp >= 0xFF10
            && cp <= 0xFF19
        ) {
            cp = '0' + (cp - 0xFF10);
        } else if (
            cp >= 0xFF21
            && cp <= 0xFF3A
        ) {
            cp = 'A' + (cp - 0xFF21);
        } else if (
            cp >= 0xFF41
            && cp <= 0xFF5A
        ) {
            cp = 'A' + (cp - 0xFF41);
        } else if (
            cp >= 'a'
            && cp <= 'z'
        ) {
            cp = (uint32_t)toupper((int)cp);
        }

        size_t written = utf8_encode_one(
            cp,
            destination + write_index,
            capacity - write_index - 1
        );
        if (!written) break;

        write_index += written;
    }

    destination[write_index] = '\0';
}

static intent_result_t empty_result(void)
{
    intent_result_t result = {
        .kind = INTENT_UNKNOWN,
        .note = "",
        .song_query = "",
        .string_id = 0,
        .tempo_bpm = 0.0f,
    };
    return result;
}

static intent_result_t kind_result(
    intent_kind_t kind
)
{
    intent_result_t result = empty_result();
    result.kind = kind;
    return result;
}

static intent_result_t note_result(
    const char *note,
    int string_id
)
{
    intent_result_t result = empty_result();
    result.kind = INTENT_START_SINGLE;
    result.string_id = string_id;
    strlcpy(
        result.note,
        note,
        sizeof(result.note)
    );
    return result;
}

static int parse_string_id(const char *text)
{
    if (!text) return 0;

    /* Long forms first so “第二弦A4” cannot be mistaken for a note alias. */
    static const struct { const char *form; int id; } FORMS[] = {
        {"第一弦", 1}, {"第1弦", 1}, {"1弦", 1}, {"一弦", 1}, {"E弦", 1},
        {"第二弦", 2}, {"第2弦", 2}, {"2弦", 2}, {"二弦", 2}, {"A弦", 2},
        {"第三弦", 3}, {"第3弦", 3}, {"3弦", 3}, {"三弦", 3}, {"D弦", 3},
        {"第四弦", 4}, {"第4弦", 4}, {"4弦", 4}, {"四弦", 4}, {"G弦", 4},
    };

    for (size_t i = 0; i < sizeof(FORMS) / sizeof(FORMS[0]); ++i) {
        if (contains(text, FORMS[i].form)) return FORMS[i].id;
    }
    return 0;
}

static const char *open_note_for_string(int string_id)
{
    switch (string_id) {
        case 1: return "E5";
        case 2: return "A4";
        case 3: return "D4";
        case 4: return "G3";
        default: return "";
    }
}

static void remove_token(char *text, const char *token)
{
    if (!text || !token || !token[0]) return;
    size_t n = strlen(token);
    char *found = NULL;
    while ((found = strstr(text, token)) != NULL) {
        memmove(found, found + n, strlen(found + n) + 1);
    }
}

static void strip_string_designators(char *text)
{
    if (!text) return;
    /* Remove explicit string words while leaving the requested pitch untouched. */
    static const char *FORMS[] = {
        "第一弦", "第1弦", "1弦", "一弦", "E弦",
        "第二弦", "第2弦", "2弦", "二弦", "A弦",
        "第三弦", "第3弦", "3弦", "三弦", "D弦",
        "第四弦", "第4弦", "4弦", "四弦", "G弦",
        "最高音弦", "最高弦", "最低音弦", "最低弦",
        NULL,
    };
    for (size_t i = 0; FORMS[i]; ++i) remove_token(text, FORMS[i]);
}

static int chinese_octave(
    const char *text
)
{
    if (!text) return -1;

    if (contains(text, "三")) return 3;
    if (contains(text, "四")) return 4;
    if (contains(text, "五")) return 5;
    if (contains(text, "六")) return 6;
    if (contains(text, "七")) return 7;
    if (contains(text, "八")) return 8;

    return -1;
}

static int english_octave(
    const char *text
)
{
    if (!text) return -1;

    if (contains(text, "THREE")) return 3;
    if (contains(text, "FOUR")) return 4;
    if (contains(text, "FIVE")) return 5;
    if (contains(text, "SIX")) return 6;
    if (contains(text, "SEVEN")) return 7;
    if (contains(text, "EIGHT")) return 8;

    return -1;
}

static bool looks_like_note_command(
    const char *text
)
{
    if (!text) return false;

    return (
        contains(text, "练习")
        || contains(text, "单音")
        || contains(text, "目标音")
        || contains(text, "音高")
        || contains(text, "NOTE")
        || strlen(text) <= 20
    );
}


static bool alias_matches(
    const char *text,
    const char *phrase,
    bool exact
)
{
    if (!text || !phrase) return false;

    if (!exact) {
        return contains(text, phrase);
    }

    if (strcmp(text, phrase) == 0) {
        return true;
    }

    static const char *PREFIXES[] = {
        "切换到", "换成", "改练",
        "练习",
        "单音",
        "目标音",
        "音高",
        NULL,
    };

    for (
        size_t index = 0;
        PREFIXES[index];
        ++index
    ) {
        size_t prefix_length =
            strlen(PREFIXES[index]);

        if (
            strncmp(
                text,
                PREFIXES[index],
                prefix_length
            ) == 0
            && strcmp(
                text + prefix_length,
                phrase
            ) == 0
        ) {
            return true;
        }
    }

    return false;
}

static bool match_note_alias(
    const char *text,
    char *canonical,
    size_t capacity
)
{
    if (!text || !canonical) return false;

    struct {
        const char *phrase;
        const char *note;
        bool exact;
    } aliases[] = {
        /* 弦名：先于“第四”之类的短别名 */
        {"第一弦", "E5", false},
        {"第1弦", "E5", false},
        {"一弦", "E5", false},
        {"最高音弦", "E5", false},
        {"最高弦", "E5", false},

        {"第二弦", "A4", false},
        {"第2弦", "A4", false},
        {"二弦", "A4", false},

        {"第三弦", "D4", false},
        {"第3弦", "D4", false},
        {"三弦", "D4", false},

        {"第四弦", "G3", false},
        {"第4弦", "G3", false},
        {"四弦", "G3", false},
        {"最低音弦", "G3", false},
        {"最低弦", "G3", false},

        /* G3 */
        {"G3", "G3", true},
        {"G三", "G3", true},
        {"机三", "G3", true},
        {"基三", "G3", true},
        {"鸡三", "G3", true},
        {"记三", "G3", true},
        {"季三", "G3", true},
        {"低音G", "G3", false},
        {"196赫兹", "G3", false},
        {"196HZ", "G3", false},

        /* D4 */
        {"D4", "D4", true},
        {"D四", "D4", true},
        {"低四", "D4", true},
        {"地四", "D4", true},
        {"弟四", "D4", true},
        {"迪四", "D4", true},
        {"的四", "D4", true},
        {"第四", "D4", true},
        {"第4", "D4", true},
        {"低音D", "D4", false},
        {"293赫兹", "D4", false},
        {"294赫兹", "D4", false},
        {"293HZ", "D4", false},
        {"294HZ", "D4", false},

        /* A4 */
        {"A4", "A4", true},
        {"A四", "A4", true},
        {"诶四", "A4", true},
        {"哎四", "A4", true},
        {"爱四", "A4", true},
        {"艾四", "A4", true},
        {"440赫兹", "A4", false},
        {"440HZ", "A4", false},

        /* E5 */
        {"E5", "E5", true},
        {"E五", "E5", true},
        {"衣五", "E5", true},
        {"一五", "E5", true},
        {"伊五", "E5", true},
        {"依五", "E5", true},
        {"易五", "E5", true},
        {"高音E", "E5", false},
        {"659赫兹", "E5", false},
        {"660赫兹", "E5", false},
        {"659HZ", "E5", false},
        {"660HZ", "E5", false},
    };

    for (
        size_t index = 0;
        index < sizeof(aliases) / sizeof(aliases[0]);
        ++index
    ) {
        if (
            alias_matches(
                text,
                aliases[index].phrase,
                aliases[index].exact
            )
        ) {
            strlcpy(
                canonical,
                aliases[index].note,
                capacity
            );
            return true;
        }
    }

    return false;
}

static bool is_explicit_song_command(
    const char *text
)
{
    if (!text || !text[0]) return false;

    if (
        contains(text, "歌曲")
        || contains(text, "乐曲")
        || contains(text, "播放")
        || contains(text, "演奏")
        || contains(text, "来一首")
    ) {
        return true;
    }

    if (
        contains(text, "小星星")
        || contains(text, "小行星")
        || contains(text, "小猩猩")
        || contains(text, "亮晶晶")
        || contains(text, "找朋友")
        || contains(text, "生日快乐")
        || contains(text, "换成")
        || contains(text, "切换到")
        || contains(text, "改练")
        || contains(text, "我想练习")
        || contains(text, "帮我练习")
    ) {
        return true;
    }

    /*
     * A generic "练习XXX" can name an arbitrary song, but control-like ASR text
     * must never fall through and become START_SONG.
     */
    if (
        strncmp(text, "练习", strlen("练习")) == 0
        && !contains(text, "下一")
        && !contains(text, "上一")
        && !contains(text, "下一个")
        && !contains(text, "上一个")
        && !contains(text, "暂停")
        && !contains(text, "继续")
        && !contains(text, "速度")
        && !contains(text, "强化")
        && !contains(text, "结束")
    ) {
        return true;
    }

    return false;
}


static void remove_all_ascii_token(
    char *text,
    const char *token
)
{
    if (!text || !token || !token[0]) return;

    size_t token_length = strlen(token);
    char *found = NULL;

    while (
        (found = strstr(text, token))
        != NULL
    ) {
        memmove(
            found,
            found + token_length,
            strlen(found + token_length) + 1
        );
    }
}

static void build_note_core(
    const char *text,
    char *core,
    size_t capacity
)
{
    if (!core || capacity == 0) return;

    strlcpy(
        core,
        text ? text : "",
        capacity
    );

    /*
     * normalize_text()会删除空格：
     *   "sharp A4" -> "SHARPA4"
     *   "flat A4"  -> "FLATA4"
     *
     * 必须先删除这些英文命令词，否则FLAT里的F、
     * PRACTICE里的A/C等会被误认为真正的音名字母。
     */
    static const char *TOKENS[] = {
        "PRACTICE",
        "TARGET",
        "PITCH",
        "NOTE",
        "SHARP",
        "FLAT",
        NULL,
    };

    for (
        size_t index = 0;
        TOKENS[index];
        ++index
    ) {
        remove_all_ascii_token(
            core,
            TOKENS[index]
        );
    }
}

static bool parse_note_from_command(
    const char *text,
    char *canonical,
    size_t capacity
)
{
    if (
        !text
        || !canonical
        || !looks_like_note_command(text)
    ) {
        return false;
    }

    /* Strip only command wrappers at the edges, never arbitrary song text. */
    char cleaned[NORMALIZED_TEXT_CAPACITY];
    strlcpy(cleaned, text, sizeof(cleaned));
    static const char *prefixes[] = {"你好小智", "小智", "请帮我", "帮我", "我想", "我要", "现在", "请", "切换到", "换到", "换成", "改练", "练习", "练", "单音", "目标音", "音高", NULL};
    bool changed;
    do {
        changed = false;
        for (size_t i=0; prefixes[i]; ++i) {
            size_t n=strlen(prefixes[i]);
            if (!strncmp(cleaned,prefixes[i],n)) {
                memmove(cleaned,cleaned+n,strlen(cleaned+n)+1); changed=true; break;
            }
        }
    } while(changed);
    static const char *suffixes[]={"这个音", "一下", "吧", "啊", NULL};
    for(size_t i=0;suffixes[i];++i){size_t n=strlen(cleaned),m=strlen(suffixes[i]);
        if(n>=m && !strcmp(cleaned+n-m,suffixes[i]))cleaned[n-m]=0;
    }
    bool alias_sharp=contains(cleaned,"升")||contains(cleaned,"SHARP")||contains(cleaned,"井号")||strchr(cleaned,'#');
    bool alias_flat=contains(cleaned,"降")||contains(cleaned,"FLAT");
    if(alias_sharp && alias_flat)return false;
    char alias_core[NORMALIZED_TEXT_CAPACITY];strlcpy(alias_core,cleaned,sizeof(alias_core));
    const char *acc[]={"升","降","SHARP","FLAT","井号","#",NULL};
    for(size_t i=0;acc[i];++i)remove_token(alias_core,acc[i]);
    char base[NOTE_NAME_CAPACITY];
    bool matched=match_note_alias(alias_core,base,sizeof(base));
    /* Fixed-do Re; absent octave means D4. Explicit octave remains preferable. */
    if(!matched){
        const char *re[]={"RE","来","瑞",NULL};
        for(size_t i=0;re[i];++i){size_t n=strlen(re[i]);
            if(strncmp(alias_core,re[i],n))continue;
            const char *tail=alias_core+n;int oct=4;
            if(*tail){
                if(strlen(tail)==1 && *tail>='3' && *tail<='8')oct=*tail-'0';
                else if(strlen(tail)==3 && chinese_octave(tail)>=0)oct=chinese_octave(tail);
                else continue;
            }
            if(oct < 3 || oct > 8)continue;
            base[0]='D';base[1]=(char)('0'+oct);base[2]='\0';
            matched=true;break;
        }
    }
    if(matched){
        char raw[NOTE_NAME_CAPACITY];
        if(alias_sharp||alias_flat)snprintf(raw,sizeof(raw),"%c%c%c",base[0],alias_sharp?'#':'b',base[1]);
        else strlcpy(raw,base,sizeof(raw));
        return note_parse(raw,canonical,capacity) && note_is_supported(canonical);
    }
    text=cleaned;
    /*
     * 先吃掉真实ASR里常见的中文/同音误识别，
     * 再走通用A~G、sharp/flat、3~8解析。
     */
    if (
        match_note_alias(
            text,
            canonical,
            capacity
        )
    ) {
        return true;
    }

    char core[128];
    build_note_core(
        text,
        core,
        sizeof(core)
    );

    char letter = '\0';
    const char *letter_position = NULL;

    for (
        const char *cursor = core;
        *cursor;
        ++cursor
    ) {
        if (
            *cursor >= 'A'
            && *cursor <= 'G'
        ) {
            letter = *cursor;
            letter_position = cursor;
            break;
        }
    }

    if (!letter_position) return false;

    int octave = -1;

    for (
        const char *cursor = letter_position + 1;
        *cursor;
        ++cursor
    ) {
        if (
            *cursor >= '3'
            && *cursor <= '8'
        ) {
            octave = *cursor - '0';
            break;
        }
    }

    if (octave < 0) {
        octave = chinese_octave(core);
    }

    if (octave < 0) {
        octave = english_octave(core);
    }

    if (octave < 0) return false;

    bool compact_flat =
        letter_position[0] != '\0'
        && letter_position[1] == 'B'
        && isdigit(
            (unsigned char)letter_position[2]
        );

    bool sharp =
        contains(text, "升")
        || contains(text, "SHARP")
        || contains(text, "井号")
        || strchr(text, '#') != NULL;

    bool flat =
        contains(text, "降")
        || contains(text, "FLAT")
        || compact_flat;

    char raw[NOTE_NAME_CAPACITY];

    char octave_char = (char)('0' + octave);

    if (sharp) {
        raw[0] = letter;
        raw[1] = '#';
        raw[2] = octave_char;
        raw[3] = '\0';
    } else if (flat) {
        raw[0] = letter;
        raw[1] = 'b';
        raw[2] = octave_char;
        raw[3] = '\0';
    } else {
        raw[0] = letter;
        raw[1] = octave_char;
        raw[2] = '\0';
    }

    if (
        !note_parse(
            raw,
            canonical,
            capacity
        )
    ) {
        return false;
    }

    return note_is_supported(canonical);
}

static void strip_song_prefix(
    const char *text,
    char *song,
    size_t capacity
)
{
    if (!song || capacity == 0) return;
    song[0] = '\0';
    if (!text) return;

    static const char *PREFIXES[] = {
        "切换到", "换成", "改练",
        "练习歌曲",
        "练习乐曲",
        "练习",
        "演奏",
        "播放",
        "来一首",
        "歌曲",
        "乐曲",
        NULL,
    };

    const char *start = text;

    for (
        size_t index = 0;
        PREFIXES[index];
        ++index
    ) {
        const char *found =
            strstr(text, PREFIXES[index]);

        if (found) {
            start =
                found
                + strlen(PREFIXES[index]);
            break;
        }
    }

    strlcpy(song, start, capacity);
}


static int chinese_digit_value(const char *p, size_t *bytes)
{
    if (!p || !bytes) return -1;
    static const struct { const char *s; int v; } DIGITS[] = {
        {"零",0},{"一",1},{"二",2},{"两",2},{"三",3},{"四",4},
        {"五",5},{"六",6},{"七",7},{"八",8},{"九",9},
    };
    for (size_t i = 0; i < sizeof(DIGITS)/sizeof(DIGITS[0]); ++i) {
        size_t n = strlen(DIGITS[i].s);
        if (strncmp(p, DIGITS[i].s, n) == 0) {
            *bytes = n;
            return DIGITS[i].v;
        }
    }
    return -1;
}

static int parse_chinese_integer(const char *text)
{
    if (!text || !text[0]) return -1;

    const char *hundred = strstr(text, "百");
    const char *ten = strstr(text, "十");
    int value = 0;

    if (hundred) {
        size_t bytes = 0;
        int d = chinese_digit_value(text, &bytes);
        value += (d > 0 ? d : 1) * 100;

        const char *after = hundred + strlen("百");
        const char *ten2 = strstr(after, "十");
        if (ten2) {
            size_t b2 = 0;
            int td = chinese_digit_value(after, &b2);
            value += (td >= 0 && after + b2 == ten2 ? td : 1) * 10;
            const char *ones = ten2 + strlen("十");
            size_t b3 = 0;
            int od = chinese_digit_value(ones, &b3);
            if (od >= 0) value += od;
        } else {
            size_t b2 = 0;
            int od = chinese_digit_value(after, &b2);
            if (od >= 0) value += od;
        }
        return value;
    }

    if (ten) {
        if (ten == text) {
            value = 10;
        } else {
            size_t bytes = 0;
            int d = chinese_digit_value(text, &bytes);
            value = (d > 0 ? d : 1) * 10;
        }
        const char *ones = ten + strlen("十");
        size_t b2 = 0;
        int od = chinese_digit_value(ones, &b2);
        if (od >= 0) value += od;
        return value;
    }

    size_t bytes = 0;
    return chinese_digit_value(text, &bytes);
}

static float parse_tempo_value(const char *text)
{
    if (!text) return 0.0f;

    for (const char *p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end != p && v >= 40 && v <= 220) {
                return (float)v;
            }
        }
    }

    const char *markers[] = {"速度", "节拍", "每分钟", "BPM", NULL};
    for (size_t i = 0; markers[i]; ++i) {
        const char *p = strstr(text, markers[i]);
        if (!p) continue;
        p += strlen(markers[i]);
        int v = parse_chinese_integer(p);
        if (v >= 40 && v <= 220) return (float)v;
    }

    int v = parse_chinese_integer(text);
    if (v >= 40 && v <= 220 && (contains(text, "拍") || contains(text, "BPM"))) {
        return (float)v;
    }

    return 0.0f;
}

static intent_result_t tempo_result(float bpm)
{
    intent_result_t result = empty_result();
    result.kind = INTENT_TEMPO_SET;
    result.tempo_bpm = bpm;
    return result;
}

intent_result_t intent_parse(const char *text)
{
    intent_result_t unknown = empty_result();

    char normalized[NORMALIZED_TEXT_CAPACITY];
    normalize_text(
        text,
        normalized,
        sizeof(normalized)
    );

    if (!normalized[0]) return unknown;

    static const char *const END_FORMS[] = {
        "结束练习",
        "结束",
        "停止练习",
        "退出练习",
        "返回主页",
        "回到主页",
        "完成练习",
        "不练了",
        NULL,
    };
    static const char *const PAUSE_FORMS[] = {
        "暂停练习",
        "暂停",
        "先停一下",
        "停一下",
        NULL,
    };
    static const char *const CONTINUE_FORMS[] = {
        "继续练习",
        "继续",
        "恢复练习",
        "恢复",
        "接着练",
        NULL,
    };
    static const char *const TEMPO_UP_FORMS[] = {
        "速度快一点", "快一点", "加快速度", "加速", "快一些", NULL,
    };
    static const char *const TEMPO_DOWN_FORMS[] = {
        "速度慢一点", "慢一点", "降低速度", "减速", "慢一些", NULL,
    };
    static const char *const TEMPO_RESET_FORMS[] = {
        "恢复原速", "恢复速度", "正常速度", "原来的速度", NULL,
    };
    static const char *const MODE_PITCH_FORMS[] = {
        "音准模式", "音准练习", "只练音准", "练音准",
        "切到音准", "切换音准", "进入音准", "音准", "只看音准",
        NULL,
    };
    static const char *const MODE_FOLLOW_FORMS[] = {
        "跟拍练习", "跟拍模式", "逐音跟拍", "逐音练习", "跟节拍", NULL,
    };
    static const char *const MODE_RHYTHM_FORMS[] = {
        "节奏模式", "节奏练习", "只练节奏", "练节奏",
        "切到节奏", "切换节奏", "进入节奏", "节奏",
        "节拍模式", "连续节奏测试", "连续测试", "只看节奏",
        NULL,
    };
    static const char *const MODE_FULL_FORMS[] = {
        "完整模式", "完整演奏", "综合模式", "综合练习", "音准节奏一起",
        "切到完整", "切换完整", "进入完整", "完整",
        "综合", "全部一起", "音准和节奏",
        NULL,
    };
    static const char *const METRONOME_ON_FORMS[] = {
        "打开节拍器", "开启节拍器", "开节拍器", "节拍器打开", NULL,
    };
    static const char *const METRONOME_OFF_FORMS[] = {
        "关闭节拍器", "关掉节拍器", "关节拍器", "节拍器关闭", NULL,
    };
    static const char *const REVIEW_START_FORMS[] = {
        "开始强化", "开始弱点强化", "练习弱点", "强化练习", "再练弱点", NULL,
    };
    static const char *const REVIEW_SKIP_FORMS[] = {
        "跳过强化", "跳过弱点强化", "不用强化", "不练弱点", "跳过复习", NULL,
    };
    /*
     * V10.3.1:
     * Short Mandarin commands are often transcribed with homophones by cloud ASR.
     * Accept common variants, but deliberately do NOT map “下一首/上一首” here.
     */
    static const char *const NEXT_FORMS[] = {
        /*
         * V10.3.2: navigation is a tiny closed command set.
         * Cloud ASR often turns “下一音” into 下一页/下一因/下一个,
         * so accept the direction word itself instead of requiring an exact noun.
         */
        "下一", "下个", "往后", "向后", "后一个", "后面一个",
        "前进一个", "前进",
        "下一音", "下一个音", "下个音", "下一音符", "下一个音符",
        "下一因", "下一阴", "下一应", "下一银", "下一英", "下一页", "下一步",
        "后一个音", "往后一个", "向后一个",
        "下一根弦", "升半音",
        NULL,
    };
    static const char *const PREVIOUS_FORMS[] = {
        "上一", "上个", "往前", "向前", "前一个", "前面一个",
        "后退一个", "后退",
        "上一音", "上一个音", "上个音", "上一音符", "上一个音符",
        "上一因", "上一阴", "上一应", "上一银", "上一英", "上一页", "上一步",
        "前一个音", "往前一个", "向前一个",
        "上一根弦", "降半音",
        NULL,
    };

    if (contains_any(normalized, END_FORMS)) {
        return kind_result(INTENT_END);
    }
    if (contains_any(normalized, PAUSE_FORMS)) {
        return kind_result(INTENT_PAUSE);
    }
    if (contains_any(normalized, CONTINUE_FORMS)) {
        return kind_result(INTENT_CONTINUE);
    }
    if (contains_any(normalized, MODE_FOLLOW_FORMS)) {
        return kind_result(INTENT_MODE_FOLLOW);
    }
    if (contains_any(normalized, MODE_PITCH_FORMS)) {
        return kind_result(INTENT_MODE_PITCH);
    }
    if (contains_any(normalized, MODE_RHYTHM_FORMS)) {
        return kind_result(INTENT_MODE_RHYTHM);
    }
    if (contains_any(normalized, MODE_FULL_FORMS)) {
        return kind_result(INTENT_MODE_FULL);
    }
    if (contains_any(normalized, METRONOME_ON_FORMS)) {
        return kind_result(INTENT_METRONOME_ON);
    }
    if (contains_any(normalized, METRONOME_OFF_FORMS)) {
        return kind_result(INTENT_METRONOME_OFF);
    }

    float requested_tempo = parse_tempo_value(normalized);
    if (requested_tempo > 0.0f) {
        return tempo_result(requested_tempo);
    }

    if (contains(normalized,"慢速重练") || contains(normalized,"慢一点重练") || contains(normalized,"放慢重练")) return kind_result(INTENT_PHRASE_SLOW_REPEAT);
    if (contains(normalized,"重练本段") || contains(normalized,"再练一遍本段") || contains(normalized,"重复本段")) return kind_result(INTENT_PHRASE_REPEAT);
    if (contains(normalized,"下一段") || contains(normalized,"下两小节")) return kind_result(INTENT_PHRASE_NEXT);
    if (contains_any(normalized, REVIEW_SKIP_FORMS)) {
        return kind_result(INTENT_REVIEW_SKIP);
    }
    if (contains_any(normalized, REVIEW_START_FORMS)) {
        return kind_result(INTENT_REVIEW_START);
    }
    if (contains_any(normalized, TEMPO_RESET_FORMS)) {
        return kind_result(INTENT_TEMPO_RESET);
    }
    if (contains_any(normalized, TEMPO_UP_FORMS)) {
        return kind_result(INTENT_TEMPO_UP);
    }
    if (contains_any(normalized, TEMPO_DOWN_FORMS)) {
        return kind_result(INTENT_TEMPO_DOWN);
    }
    if (contains_any(normalized, NEXT_FORMS)) {
        return kind_result(INTENT_NEXT);
    }
    if (contains_any(normalized, PREVIOUS_FORMS)) {
        return kind_result(INTENT_PREVIOUS);
    }

    const char *known = NULL;
    unsigned known_count = 0;
    if (contains(normalized,"小星星") || contains(normalized,"小行星") || contains(normalized,"小猩猩") || contains(normalized,"亮晶晶")) { known="小星星"; ++known_count; }
    if (contains(normalized,"找朋友")) { known="找朋友"; ++known_count; }
    if (contains(normalized,"生日快乐")) { known="生日快乐"; ++known_count; }
    if (known_count > 1) return unknown;
    if (contains(normalized,"重新开始") || contains(normalized,"从头练习")) {
        if (known) return unknown; /* avoid restarting the wrong named song */
        return kind_result(INTENT_RESTART);
    }
    if (known) {
        intent_result_t result=empty_result(); result.kind=INTENT_START_SONG;
        strlcpy(result.song_query,known,sizeof(result.song_query)); return result;
    }

    /*
     * Explicit string + pitch is now a first-class command:
     *   “二弦A4” -> A4 on string 2 (open A)
     *   “三弦A4” -> A4 on string 3 (stopped D string)
     * The pitch must not be overwritten by the old “二弦=A4 / 三弦=D4” aliases.
     */
    int string_id = parse_string_id(normalized);
    char note_command[NORMALIZED_TEXT_CAPACITY];
    strlcpy(note_command, normalized, sizeof(note_command));
    if (string_id > 0) strip_string_designators(note_command);

    char note[NOTE_NAME_CAPACITY];
    if (
        parse_note_from_command(
            note_command,
            note,
            sizeof(note)
        )
    ) {
        return note_result(note, string_id);
    }

    /* Saying only “二弦 / 三弦 …” still means that string's open note. */
    if (string_id > 0) {
        return note_result(open_note_for_string(string_id), string_id);
    }

    /*
     * V3.1：
     * 未识别文本不再自动当歌曲。
     * 只有明确歌曲表达才允许访问曲库。
     */
    if (is_explicit_song_command(normalized)) {
        char song_query[96];

        strip_song_prefix(
            normalized,
            song_query,
            sizeof(song_query)
        );

        if (
            song_query[0]
            && strlen(song_query) >= 2
        ) {
            intent_result_t result =
                empty_result();

            result.kind =
                INTENT_START_SONG;

            strlcpy(
                result.song_query,
                song_query,
                sizeof(result.song_query)
            );

            return result;
        }
    }

    return unknown;
}
