/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

/* disable warnings about old C89 functions in MSVC */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning (push)
/* disable warning about single line comments in system headers */
#pragma warning (disable : 4001)
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#include "cJSON.h"

/* define our own boolean type */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* define isnan and isinf for ANSI C, if in C99 or above, isnan and isinf has been defined in math.h */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)
#else
#define NAN 0.0/0.0
#endif
#endif

typedef struct {
    const unsigned char *json;
    size_t position;
} error;
static error global_error = { NULL, 0 };

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) (global_error.json + global_error.position);
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item))
    {
        return NULL;
    }

    return item->valuestring;
}

CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item))
    {
        return (double) NAN;
    }

    return item->valuedouble;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and header files */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}

/* Case insensitive string comparison, doesn't consider two NULL pointers equal though */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL))
    {
        return 1;
    }

    if (string1 == string2)
    {
        return 0;
    }

    for(; tolower(*string1) == tolower(*string2); (void)string1++, string2++)
    {
        if (*string1 == '\0')
        {
            return 0;
        }
    }

    return tolower(*string1) - tolower(*string2);
}

typedef struct internal_hooks
{
    void *(CJSON_CDECL *allocate)(size_t size);
    void (CJSON_CDECL *deallocate)(void *pointer);
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dllimport '...' is not static */
static void * CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
static void * CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* strlen of character literals resolved at compile time */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

/*字符串复制函数，把外部字符串复制到堆中，由cJSON节点持有*/
static unsigned char* cJSON_strdup(const unsigned char* string, const internal_hooks * const hooks)
{   /* 实现内存所有权转移：
     cJSON_strdup创建了新内存块
     cJSON节点现在拥有这块内存的所有权，在cJSON_Delete时必须释放*/
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL)//空指针检查
    {
        return NULL;
    }

    length = strlen((const char*)string) + sizeof("");//长度计算
    copy = (unsigned char*)hooks->allocate(length);//抽象内存分配，允许调用者自定义内存管理
    if (copy == NULL)
    {
        return NULL;//分配失败返回NULL，调用者处理
    }
    memcpy(copy, string, length);//使用memcpy实现内存复制

    return copy;
}

/*初始化钩子实现自定义内存管理*/
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{

    /*检查hooks是否为NULL
    如果用户未提供自定义的钩子
    重置全局钩子为默认的内存分配函数malloc、free、realloc作为内存管理的默认方案*/
    if (hooks == NULL)
    {
        /* Reset hooks */
        global_hooks.allocate = malloc;
        global_hooks.deallocate = free;
        global_hooks.reallocate = realloc;
        return;
    }
   /*设置自定义钩子*/
    global_hooks.allocate = malloc;//global_hooks.allocate 初始设为 malloc，以便在没有自定义分配函数时使用。
    if (hooks->malloc_fn != NULL)//检查用户提供的分配函数
    {
        global_hooks.allocate = hooks->malloc_fn;
        /*若用户定义了 malloc_fn，则将 global_hooks.allocate更新为该自定义函数
        使用户可以通过提供自定义函数来控制内存分配*/

    }

    global_hooks.deallocate = free;//默认释放函数为free
    if (hooks->free_fn != NULL)
    {
        global_hooks.deallocate = hooks->free_fn;//如果用户提供了free_fn，则更新使用这个自定义释放函数
    }

    /* use realloc only if both free and malloc are used */
    global_hooks.reallocate = NULL;//条件设置reallocate
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free))
    {
        global_hooks.reallocate = realloc;
        /*只有当全局的分配和释放函数是标准的malloc和free 时
        将global_hooks.reallocate 设置为 realloc
        在自定义的内存管理场景下，realloc不一定适用*/
    }
}

/* Internal constructor. */
/*内部创建新JSON项
输入参数指向一个包含内存管理函数指针的结构体。*/
static cJSON *cJSON_New_Item(const internal_hooks * const hooks)
{
    cJSON* node = (cJSON*)hooks->allocate(sizeof(cJSON));
    //使用hooks->allocate函数为新的cJSON节点分配内存，并将返回的内存指针强制转换为cJSON类型
    if (node)//通过检查node确保内存分配成功
    {
        memset(node, '\0', sizeof(cJSON));//使用memset将分配的所有字段都初始化为零
    }

    return node;//内存分配成功并且节点被清零返回新创建的cJSON节点，分配失败返回 NULL
}

/* Delete a cJSON structure. */
/*删除cJson对象
结合递归和迭代处理多层嵌套的JSON结构
确保所有层级的节点都被正确删除*/
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    //创建next指针以保存下一个节点的引用，防止在删除当前节点时丢失对其他节点的引用
    while (item != NULL)
    {
        /*通过while循环遍历所有的cJSON节点
        直到指针为空
        每次循环时使用next保存当前节点的下一个节点*/
        next = item->next;

        //销毁子节点
        /*通过位与运算检查item是否是引用类型
        如果不是引用类型且存在子节点
        递归调用cJSON_Delete删除子节点，确保所有子节点在删除父节点前被正确处理*/
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }

        //释放值字符串
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))//检查valuestring是否非空且不是引用类型
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
            /*用global_hooks.deallocate释放之前分配的内存
            并将指针设置为NULL，避免悬空指针*/
        }

        //释放项字符串
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {   //检查是否是常量字符串，并释放相应的内存
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        global_hooks.deallocate(item);//通过global_hooks.deallocate释放当前的cJSON节点内存
        item = next;//移动指针到下一个节点继续循环，直到所有节点处理完成
    }
}

/* get the decimal point character of the current locale */
//小数点字符的国际化处理
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();//获取当前locale的数字格式信息 
    return (unsigned char) lconv->decimal_point[0];
#else
    return '.';
#endif
}

 /*parse_buffer结构体，用于跟踪解析的进度与状态*/
typedef struct
{
    const unsigned char *content;
    size_t length;
    size_t offset;//解析的偏移量，表示在解析过程中已经读取的字节数，确保解析从正确的位置进行
    size_t depth; //记录当前解析的嵌套深度，明确结构层级
    /* How deeply nested (in arrays/objects) is the input at the current offset. */
    internal_hooks hooks;//钩子结构体，允许用户自定义内存操作函数
} parse_buffer;

//宏定义保证安全性，避免内存越界
/* check if the given size is left to read in a given parse buffer (starting with 1) */
/*检查在给定的解析缓冲区中是否还有足够的字节可以读取*/
#define can_read(buffer, size) ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
/* check if the buffer can be accessed at the given index (starting with 0) */
/*检查解析缓冲区在给定索引处是否可访问*/
#define can_access_at_index(buffer, index) ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
/*can_access_at_index的反向检查，用于判断是否无法在给定索引访问缓冲区*/
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* get a pointer to the buffer at the position */
/*从offset位置开始获取当前缓冲区内容指针
便于直接访问解析当前处于的内存位置*/
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* Parse the input text to generate a number, and populate the result into item. */
//处理数字解析函数
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;// 存储转换后的双精度浮点值 
    unsigned char *after_end = NULL;//strtod转换结束位置指针
    unsigned char *number_c_string; //动态分配的临时数字字符串缓冲区 
    unsigned char decimal_point = get_decimal_point();//获取当前locale的小数点字符 
    size_t i = 0;
    size_t number_string_length = 0;
    cJSON_bool has_decimal_point = false;//标记是否包含小数点

    /* 输入参数有效性检查，防止空指针解引用 */
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;
    }
    /*第一遍扫描确定数字字符串长度*/
    /* copy the number into a temporary buffer and replace '.' with the decimal point
     * of the current locale (for strtod)
     * This also takes care of '\0' not necessarily being available for marking the end of the input */
    for (i = 0; can_access_at_index(input_buffer, i); i++)
    {
        /* 判断当前字符是否是数字的合法组成部分 */
        switch (buffer_at_offset(input_buffer)[i])
        {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '+':
            case '-':
            case 'e':
            case 'E':
                number_string_length++;
                break;
            /* 小数点特殊处理，需要本地化转换 */
            case '.':
                number_string_length++;
                has_decimal_point = true;///标记需要本地化处理 
                break;

            default:
                goto loop_end;///任何其他字符都表示数字结束，goto语句跳出循环，继续后续处理
        }
    }
loop_end:
    /* malloc for temporary buffer, add 1 for '\0' */
    //动态分配一个临时缓冲区来存储数字字符串，长度为扫描到的数字字符串长度加1（用于字符串结束符'\0'）
    number_c_string = (unsigned char *) input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL)
    {
        return false; /* allocation failure */
    }
   //使用memcpy将扫描到的数字字符串从输入缓冲区复制到临时缓冲区
    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0';//手动添加字符串终止符

    /*对小数点的本地兼容化处理
    如果数字字符串中包含小数点，遍历临时缓冲区
    将所有的'.'替换为当前locale的小数点字符，确保strtod能够正确解析数字字符串*/
    if (has_decimal_point)
    {
        for (i = 0; i < number_string_length; i++)
        {
            if (number_c_string[i] == '.')
            {
                /* replace '.' with the decimal point of the current locale (for strtod) */
                /* 将JSON标准小数点替换为当前locale的小数点 */
                number_c_string[i] = decimal_point;
            }
        }
    }
    //使用strtod将数字字符串转换为双精度浮点数，并获取转换结束位置
    number = strtod((const char*)number_c_string, (char**)&after_end);
    if (number_c_string == after_end)///错误检查，如果after_end == number_c_string，说明转换失败
    {
        /* free the temporary buffer */
        /* 释放临时缓冲区，防止内存泄漏 */
        input_buffer->hooks.deallocate(number_c_string);
        return false; /* parse_error */

    }
     /* 存储解析获得的数字双精度值 */
    item->valuedouble = number;

    /* use saturation in case of overflow */
    /*整数溢出保护：
     使用饱和截断来处理double转int时的溢出问题
     将超出范围的值截断为INT_MAX/INT_MIN*/
    if (number >= INT_MAX)
    {
        item->valueint = INT_MAX;//正数上限截断
    }
    else if (number <= (double)INT_MIN)
    {
        item->valueint = INT_MIN;//负数下限截断
    }
    else
    {
        item->valueint = (int)number;//安全转换
    }

    item->type = cJSON_Number;//将节点类型设置为数字
     /*缓冲区指针推进
     after_end指向临时缓冲区中的结束位置，after_end - number_c_string 计算出实际消耗的数字字符数
     直接更新input_buffer的偏移量，跳过已解析的数字
     */
    input_buffer->offset += (size_t)(after_end - number_c_string);

    /* free the temporary buffer */
    /* 释放临时缓冲区，防止内存泄漏 */
    input_buffer->hooks.deallocate(number_c_string);
    return true;
}

/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
/*设置数字值的辅助函数
  用于维护cJSON数字节点的双重表示一致性，让valueint和valuedouble保持一致
  并处理整数溢出和类型转换 */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;//正数上限截断
    }
    else if (number <= (double)INT_MIN)
    {
        object->valueint = INT_MIN;//负数下限截断
    }
    else
    {
        object->valueint = (int)number;//安全转换
    }

    return object->valuedouble = number;
}

/* Note: when passing a NULL valuestring, cJSON_SetValuestring treats this as an error and return NULL */
/*设置字符串值的辅助函数
  用于更新cJSON字符串节点的值字符串，处理内存管理和安全性检查
  确保只有非引用类型的字符串节点才能设置值字符串，避免内存泄漏和重叠内存复制问题*/
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* if object's type is not cJSON_String or is cJSON_IsReference, it should not set valuestring */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference))//检查对象是否为NULL,字符串或引用类型
    {
        return NULL;
    }
    /* return NULL if the object is corrupted or valuestring is NULL */
    /*检查数据完整性：
    如果对象的valuestring为NULL或者传入的valuestring参数为NULL
    函数返回NULL表示设置失败*/
    if (object->valuestring == NULL || valuestring == NULL)
    {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)//如果新字符串长度不超过当前字符串长度，直接覆盖原有内存，重用已有缓冲区
    {
        /* strcpy does not handle overlapping string: [X1, X2] [Y1, Y2] => X2 < Y1 or Y2 < X1 */
        if (!( valuestring + v1_len < object->valuestring || object->valuestring + v2_len < valuestring ))
        {
            return NULL;
        }
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
    //新字符串长度超过当前字符串长度，需要重新分配内存
    copy = (char*) cJSON_strdup((const unsigned char*)valuestring, &global_hooks);//使用cJSON_strdup将内存所有权转移到cJSON节点
    if (copy == NULL)
    {
        return NULL;
    }
    if (object->valuestring != NULL)//释放原有内存
    {
        cJSON_free(object->valuestring);
    }
    object->valuestring = copy;//更新节点的值字符串指针
    return copy;
}
  

/*cJSON_Print→ print_value → print_string / print_number → 
printbuffer → 最终输出
printbuffer结构体用于在打印JSON字符串时管理输出缓冲区*/
typedef struct
{
    unsigned char *buffer;
    size_t length;//缓冲区的总长度，即容量上限
    size_t offset;//当前写入位置的偏移量，表示已经写入的数据长度
    size_t depth; /* current nesting depth (for formatted printing) */
    //记录当前的嵌套深度，用于格式化输出时控制缩进和换行
    cJSON_bool noalloc;
    /*标记是否禁止自动扩展缓冲区，以便设置不同的内存状态
    noalloc=true禁止扩容，采用预分配缓冲区的静态模式
    noalloc=false允许扩容，动态缓冲区自动扩容，无限写入*/
    cJSON_bool format; /* is this print a formatted print */
    //标记是否进行格式化打印
    internal_hooks hooks;//钩子结构体，允许用户自定义内存相关函数，确保在打印过程中使用一致的内存管理策略
} printbuffer;


/* realloc printbuffer if necessary to have at least "needed" bytes more */
//内存扩容函数,确保printbuffer有足够的空间来写入新的数据
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((p == NULL) || (p->buffer == NULL))//结构体完整性检查
    {
        return NULL;
    }

    if ((p->length > 0) && (p->offset >= p->length))//检查写入位置是否有效
    {
        /* make sure that offset is valid */
        return NULL;
    }

    if (needed > INT_MAX)//检查请求的空间大小是否超过INT_MAX，防止整数溢出
    {
        /* sizes bigger than INT_MAX are currently not supported */
        return NULL;
    }

    needed += p->offset + 1;//计算总的所需空间，包括当前偏移量和新数据长度，外加1字节用于字符串结束符'\0'
    if (needed <= p->length)
    {
        return p->buffer + p->offset;//如果当前缓冲区已经有足够的空间，直接返回当前写入位置的指针，无需扩容
    }

    if (p->noalloc) {
        return NULL;
    }

    /* calculate new buffer size */
    if (needed > (INT_MAX / 2))//2倍扩容处理
    {
        /* overflow of int, use INT_MAX if possible */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        newsize = needed * 2;
    }

    if (p->hooks.reallocate != NULL)//有reallocate钩子，使用它进行扩容
    {
        /* reallocate with realloc if available */
        newbuffer = (unsigned char*)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL)//扩容失败，释放原有缓冲区并重置状态
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
    }
    else
    {
        /* otherwise reallocate manually */
        //没有reallocate钩子，手动分配新缓冲区并复制原有数据
        newbuffer = (unsigned char*)p->hooks.allocate(newsize);//allocate分配新内存
        if (!newbuffer)
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }

        memcpy(newbuffer, p->buffer, p->offset + 1);//memcpy复制旧数据
        p->hooks.deallocate(p->buffer);//deallocate释放旧内存
    }
    p->length = newsize;
    p->buffer = newbuffer;//重置printbuffer的长度和缓冲区指针

    return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer and update the offset */
//计算字符串长度并更新printbuffer的偏移量
static void update_offset(printbuffer * const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL))
    {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;

    buffer->offset += strlen((const char*)buffer_pointer);
}



/* securely comparison of floating-point variables */
//双精度浮点数的比较
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/* Render the number nicely from the given item into a string. */
/*数值序列化函数
将数字cJSON转换为字符串，处理格式化和本地化问题
与parse_number对称*/
static cJSON_bool print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;
    int length = 0;
    size_t i = 0;
    unsigned char number_buffer[26] = {0}; /* temporary buffer to print the number into */
    unsigned char decimal_point = get_decimal_point();
    double test = 0.0;

    if (output_buffer == NULL)//输出缓冲区检查，确保有有效的printbuffer来写入结果
    {
        return false;
    }

    /* This checks for NaN and Infinity */
    /*使用isnan和isinf检查数字是否为NaN或Infinity
     如果是这些特殊值，直接将字符串"null"写入输出缓冲区，符合JSON规范对这些值的处理方式
     其他情况下，继续进行正常的数字格式化处理*/
    if (isnan(d) || isinf(d))
    {
        length = sprintf((char*)number_buffer, "null");
    }
    else if(d == (double)item->valueint)//如果数字的双精度表示与整数表示相等，说明这个数字可以安全地表示为整数
    {
        length = sprintf((char*)number_buffer, "%d", item->valueint);
    }
    else
    {
        /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */
        /*首先尝试使用15位小数的精度来格式化数字
         通过限制小数位数得到更符合预期的输出*/
        length = sprintf((char*)number_buffer, "%1.15g", d);

        /* Check whether the original double can be recovered */
        /*使用sscanf将格式化后的字符串转换回双精度数，并与原始数字进行比较，验证格式化的准确性
         如果得到的数字与原始数字不匹配
         增加到17位小数的精度进行重新格式化*/
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* If not, print with 17 decimal places of precision */
            length = sprintf((char*)number_buffer, "%1.17g", d);
        }
    }

    /* sprintf failed or buffer overrun occurred */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1)))
    //检查sprintf的返回值，确保格式化成功且没有发生缓冲区溢出
    {
        return false;
    }

    /* reserve appropriate space in the output */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    //调用ensure函数确保输出缓冲区有足够的空间来写入格式化后的数字字符串
    if (output_pointer == NULL)
    {
        return false;
    }

    /* copy the printed number to the output and replace locale
     * dependent decimal point with '.' */
    for (i = 0; i < ((size_t)length); i++)
    //将格式化后的数字字符串从number_buffer复制到输出缓冲区，同时将本地化的小数点字符替换为JSON标准的小数点'.'
    {
        if (number_buffer[i] == decimal_point)
        {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';

    output_buffer->offset += (size_t)length;//更新输出缓冲区的偏移量

    return true;
}

/* parse 4 digit hexadecimal number */
//解析4位十六进制数字
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        /* parse digit */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* invalid */
        {
            return 0;
        }

        if (i < 3)
        {
            /* shift left to make place for the next nibble */
            h = h << 4;
        }
    }

    return h;
}

/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
//将UTF-16字面量转换为UTF-8
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;
    unsigned int first_code = 0;
    const unsigned char *first_sequence = input_pointer;
    unsigned char utf8_length = 0;
    unsigned char utf8_position = 0;
    unsigned char sequence_length = 0;
    unsigned char first_byte_mark = 0;

    if ((input_end - first_sequence) < 6)
    {
        /* input ends unexpectedly */
        goto fail;
    }

    /* get the first utf16 sequence */
    first_code = parse_hex4(first_sequence + 2);

    /* check that the code is valid */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF)))
    {
        goto fail;
    }

    /* UTF16 surrogate pair */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* \uXXXX\uXXXX */

        if ((input_end - second_sequence) < 6)
        {
            /* input ends unexpectedly */
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            /* missing second half of the surrogate pair */
            goto fail;
        }

        /* get the second utf16 sequence */
        second_code = parse_hex4(second_sequence + 2);
        /* check that the code is valid */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            /* invalid second half of the surrogate pair */
            goto fail;
        }


        /* calculate the unicode codepoint from the surrogate pair */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        sequence_length = 6; /* \uXXXX */
        codepoint = first_code;
    }

    /* encode as UTF-8
     * takes at maximum 4 bytes to encode:
     * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint < 0x80)
    {
        /* normal ascii, encoding 0xxxxxxx */
        utf8_length = 1;
    }
    else if (codepoint < 0x800)
    {
        /* two bytes, encoding 110xxxxx 10xxxxxx */
        utf8_length = 2;
        first_byte_mark = 0xC0; /* 11000000 */
    }
    else if (codepoint < 0x10000)
    {
        /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_length = 3;
        first_byte_mark = 0xE0; /* 11100000 */
    }
    else if (codepoint <= 0x10FFFF)
    {
        /* four bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_length = 4;
        first_byte_mark = 0xF0; /* 11110000 */
    }
    else
    {
        /* invalid unicode codepoint */
        goto fail;
    }

    /* encode as utf8 */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--)
    {
        /* 10xxxxxx */
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* encode first byte */
    if (utf8_length > 1)
    {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    }
    else
    {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;

    return sequence_length;

fail:
    return 0;
}

/* Parse the input text into an unescaped cinput, and populate item. */
//解析字符串函数
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;//输入缓冲区指针，指向字符串内容的开始位置
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;//用于扫描字符串直到结束引号
    unsigned char *output_pointer = NULL;//输出缓冲区指针，指向解码后的字符串位置
    unsigned char *output = NULL;

    /* not a string */
    if (buffer_at_offset(input_buffer)[0] != '\"')//检查的第一个字符是否为双引号，如果不是，说明不是一个有效的JSON字符串
    {
        goto fail;
    }

    {   /*第一遍扫描；计算输出字符串的长度
         通过扫描输入字符串直到遇到未转义的结束引号，计算出输出字符串的实际长度
         以在分配内存时能够准确地知道需要多少空间来存储解码后的字符串*/
        /* calculate approximate size of the output (overestimate) */
        size_t allocation_length = 0;
        size_t skipped_bytes = 0;
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"'))//扫描输入字符串直到遇到未转义的结束引号，确保不超过输入缓冲区的长度
        {
            /* is escape sequence */
            if (input_end[0] == '\\')//如果当前字符是反斜杠，跳过转义字符和被转义的字符
            {
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length)
                {
                    /* prevent buffer overflow when last input character is a backslash */
                    goto fail;
                }
                skipped_bytes++;
                input_end++;
            }
            input_end++;
        }
        //扫描结束时没有找到结束引号，说明字符串未正确结束
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"'))
        {
            goto fail; /* string ended unexpectedly */
        }

        /* This is at most how much we need for the output */
        /*计算输出字符串的长度，总字符数 - 转义符数 = 实际需要的字节数*/
        allocation_length = (size_t) (input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        output = (unsigned char*)input_buffer->hooks.allocate(allocation_length + sizeof(""));//分配内存来存储解码后的字符串
        if (output == NULL)
        {
            goto fail; /* allocation failure */
        }
    }

    output_pointer = output;//初始化输出指针，准备写入解码后的字符串
    /* loop through the string literal */
    /*第二次扫描：解码转义序列，生成实际字符串*/
    while (input_pointer < input_end)
    {
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;// 普通字符，直接复制
        }
        /* escape sequence */
        else//如果遇到转义符，处理转义序列，根据转义字符的类型进行相应的解码
        {
            unsigned char sequence_length = 2;
            if ((input_end - input_pointer) < 1)//检查转义序列是否完整
            {
                goto fail;
            }

            switch (input_pointer[1])//根据转义字符的类型进行解码
            {
                case 'b':
                    *output_pointer++ = '\b';
                    break;
                case 'f':
                    *output_pointer++ = '\f';
                    break;
                case 'n':
                    *output_pointer++ = '\n';
                    break;
                case 'r':
                    *output_pointer++ = '\r';
                    break;
                case 't':
                    *output_pointer++ = '\t';
                    break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;

                /* UTF-16 literal */
                case 'u'://如果转义序列是Unicode，调用utf16_literal_to_utf8函数将其转换为UTF-8编码并输出
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length == 0)
                    {
                        /* failed to convert UTF16-literal to UTF-8 */
                        goto fail;
                    }
                    break;

                default:
                    goto fail;
            }
            input_pointer += sequence_length;//跳过转义序列，继续扫描下一个字符
        }
    }

    /* zero terminate the output */
    *output_pointer = '\0';//添加字符串结束符'\0'

    //设置cJSON节点的类型和值字符串指针，转移内存所有权，完成字符串解析
    item->type = cJSON_String;
    item->valuestring = (char*)output;
    //所有权转移到cJSON节点

    input_buffer->offset = (size_t) (input_end - input_buffer->content);//更新输入缓冲区的偏移量，跳过已解析的字符串内容
    input_buffer->offset++;//跳过结束引号

    return true;

fail://解析失败处理，释放已分配的内存并重置状态
    if (output != NULL)//解析失败，释放内存
    {
        input_buffer->hooks.deallocate(output);
        output = NULL;
    }

    if (input_pointer != NULL)//输入指针不为NULL，恢复offset停在错误位置
    {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }

    return false;
}

/* Render the cstring provided to an escaped version that can be printed. */
/*字符串序列化函数
 将C字符串转换为JSON字符串
 作为parse_string的逆操作*/
static cJSON_bool print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;

    if (output_buffer == NULL)//确保printbuffer不为空
    {
        return false;
    }

    /* empty string */
    if (input == NULL)//处理空字符串
    {
        output = ensure(output_buffer, sizeof("\"\""));//ensure请求3字节
        if (output == NULL)
        {
            return false;
        }
        strcpy((char*)output, "\"\"");//strcpy写入空字符串的JSON表示

        return true;
    }

    /* set "flag" to 1 if something needs to be escaped */
    //第一遍扫描：统计需要转义的字符数量，以便计算输出字符串的长度
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                /* one character escape sequence */
                //统计每个需要转义的字符会增加一个反斜杠，escape_characters增加1
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    /* UTF-16 escape sequence uXXXX */
                    //对于控制字符使用Unicode转义序列\uXXXX来表示，escape_characters增加5
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;//计算输出字符串的总长度，包括原始字符和转义字符

     /* allocate space for the output */
     //分配内存，长度为计算出的输出字符串长度加上两对引号和字符串结束符的空间
     output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL)
    {
        return false;
    }

    /* no characters have to be escaped */
    //如果没有需要转义的字符，直接将输入字符串用引号包裹起来写入输出缓冲区
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    output[0] = '\"';
    output_pointer = output + 1;//初始化输出指针，准备写入转义后的字符串内容
    /* copy the string */
    //第二遍扫描：生成转义后的字符串，处理每个字符，根据需要添加转义符并写入输出缓冲区
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))//如果当前字符不需要转义，直接复制
        {
            /* normal character, copy */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* character needs to be escaped */
            //需要转义的字符，首先写入一个反斜杠，然后根据转义字符的类型写入相应的转义序列
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer = '\\';
                    break;
                case '\"':
                    *output_pointer = '\"';
                    break;
                case '\b':
                    *output_pointer = 'b';
                    break;
                case '\f':
                    *output_pointer = 'f';
                    break;
                case '\n':
                    *output_pointer = 'n';
                    break;
                case '\r':
                    *output_pointer = 'r';
                    break;
                case '\t':
                    *output_pointer = 't';
                    break;
                default:
                    /* escape and print as unicode codepoint */
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 4;
                    break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/* Invoke print_string_ptr (which is useful) on an item. */
//调用print_string_ptr函数将cJSON字符串节点的值字符串进行转义和格式化
static cJSON_bool print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/* Predeclare these prototypes. */
//声明解析和打印函数
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer);

/* Utility to jump whitespace and cr/lf */
//跳过输入缓冲区中的空白字符和控制字符
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL))
    {
        return NULL;
    }

    if (cannot_access_at_index(buffer, 0))
    {
        return buffer;
    }

    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32))//跳过空白字符和控制字符，直到遇到非空白字符或达到缓冲区的末尾
    {
       buffer->offset++;
    }

    if (buffer->offset == buffer->length)
    {
        buffer->offset--;
    }

    return buffer;
}

/* skip the UTF-8 BOM (byte order mark) if it is at the beginning of a buffer */
//跳过UTF-8 BOM如果它位于缓冲区的开头
static parse_buffer *skip_utf8_bom(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0))
    {
        return NULL;
    }

    if (can_access_at_index(buffer, 4) && (strncmp((const char*)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0))
    {
        buffer->offset += 3;
    }

    return buffer;
}

//提供不同的选项来控制解析行为
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value)
    {
        return NULL;
    }

    /* Adding null character size due to require_null_terminated. */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

/* Parse an object - create a new root, and populate. */
/*解析JSON字符串，创建cJSON对象树
作为带长度限制、BOM处理、精确错误定位解析接口*/
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    parse_buffer buffer = { 0, 0, 0, 0, { 0, 0, 0 } };
    cJSON *item = NULL;

    /* reset error position */
    //解析错误位置重置，以便在解析过程中记录错误
    global_error.json = NULL;
    global_error.position = 0;

    /*与cJSON_Parse的核心区别
    cJSON_Parse信任字符串以'\0'结尾*/
    if (value == NULL || 0 == buffer_length)
    {
        goto fail;
    }

    buffer.content = (const unsigned char*)value;
    buffer.length = buffer_length;
    buffer.offset = 0;
    buffer.hooks = global_hooks;

    item = cJSON_New_Item(&global_hooks);//创建新的cJSON节点作为解析结果的根节点
    if (item == NULL) /* memory fail */
    {
        goto fail;
    }

     /*调用skip_utf8_bom跳过UTF-8 BOM，
     然后调用buffer_skip_whitespace跳过前导空白字符
     最后调用parse_value开始解析JSON字符串*/
    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer))))
    {
        /* parse failure. ep is set. */
        goto fail;
    }

    /* if we require null-terminated JSON without appended garbage, skip and then check for a null terminator */
    //如果require_null_terminated为true，表示要求JSON字符串必须以'\0'结尾且没有附加的垃圾数据
    if (require_null_terminated)
    {
        buffer_skip_whitespace(&buffer);
        if ((buffer.offset >= buffer.length) || buffer_at_offset(&buffer)[0] != '\0')//检查当前偏移位置是否已经达到输入缓冲区的末尾，或者当前字符是否为'\0'，如果不是，说明JSON字符串没有正确结束
        {
            goto fail;
        }
    }
    if (return_parse_end)//设置return_parse_end参数指向解析结束的位置
    {
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    return item;

fail:
    if (item != NULL)//解析失败，删除已创建的cJSON节点，释放内存
    {
        cJSON_Delete(item);
    }

    if (value != NULL)//如果输入字符串不为NULL，设置全局错误信息，记录错误位置
    {
        error local_error;
        local_error.json = (const unsigned char*)value;
        local_error.position = 0;

        if (buffer.offset < buffer.length)//如果当前偏移位置在输入缓冲区的范围内，设置错误位置为当前偏移位置，否则设置为输入缓冲区的末尾
        {
            local_error.position = buffer.offset;
        }
        else if (buffer.length > 0)
        {
            local_error.position = buffer.length - 1;
        }

        if (return_parse_end != NULL)//如果return_parse_end参数不为NULL，设置它指向错误位置
        {
            *return_parse_end = (const char*)local_error.json + local_error.position;
        }

        global_error = local_error;//将局部错误信息保存到全局变量global_error中
    }

    return NULL;
}


/*感觉cJSON_ParseWithOpts和cJSON_ParseWithLengthOpts像C++中函数重载的C处理*/
//默认的解析接口，调用cJSON_ParseWithOpts并传递默认参数
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}
//带长度限制的解析接口，调用cJSON_ParseWithLengthOpts并传递buffer_length
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))//定义宏cjson_min，用于计算两个值中的较小者

/*print流程：创建并配置printbuffer
 调用print_value递归遍历整个JSON树
 根据hooks配置，选择最优方式返回结果
 */
static unsigned char *print(const cJSON * const item, cJSON_bool format, const internal_hooks * const hooks)
{
    static const size_t default_buffer_size = 256;//默认缓冲区大小，初始分配256字节的内存来存储打印结果
    printbuffer buffer[1];
    /*栈上printbuffer用于存储打印过程中使用的缓冲区和相关信息
     buffer[1]自动退化为指针*/
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));//初始化printbuffer结构体，将所有字段设置为0

    /* create buffer */
    buffer->buffer = (unsigned char*) hooks->allocate(default_buffer_size);//调用hooks的allocate函数分配内存
    //初始化printbuffer
    buffer->length = default_buffer_size;
    buffer->format = format;
    buffer->hooks = *hooks;
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* print the value */
    if (!print_value(item, buffer))//调用print_value函数递归遍历整个JSON树
    {
        goto fail;
    }
    update_offset(buffer);
    /*print_value 在写入过程中不断更新offset，但某些路径可能直接操作buffer而不更新offset
     调用update_offset函数更新printbuffer的偏移量
     确保offset反映真实写入位置*/

    /* check if reallocate is available */
    if (hooks->reallocate != NULL)//根据hooks配置，选择最优方式返回结果
    {//路径1：有reallocate钩子，直接在原缓冲区上调整大小以适应最终字符串的长度
        printed = (unsigned char*) hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            goto fail;
        }
        buffer->buffer = NULL;//将buffer指针置空，表示所有权已经转移给printed变量
    }
    else /* otherwise copy the JSON over to a new buffer */
    //路径2：没有reallocate钩子，分配一个新的缓冲区，复制打印结果并添加字符串结束符
    {
        printed = (unsigned char*) hooks->allocate(buffer->offset + 1);
        if (printed == NULL)
        {
            goto fail;
        }
        memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
        printed[buffer->offset] = '\0'; /* just to be sure */

        /* free the buffer */
        hooks->deallocate(buffer->buffer);//释放原缓冲区的内存
        buffer->buffer = NULL;
    }

    return printed;

fail:
    if (buffer->buffer != NULL)//解析失败，释放原缓冲区的内存
    {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    if (printed != NULL)//释放已经分配的新缓冲区
    {
        hooks->deallocate(printed);
        printed = NULL;
    }

    return NULL;
}

/* Render a cJSON item/entity/structure to text. */
/*提供不同的选项来控制打印行为，cJSON_Print完全托管,内部决定一切
cJSON_PrintBuffered用户有建议权
cJSON_PrintPreallocated完全用户控制
*/
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)//传递format参数为true，格式化输出
{
    return (char*)print(item, true, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)//传递format参数为false，不格式化输出
{
    return (char*)print(item, false, &global_hooks);
}

//提供允许指定预分配缓冲区大小和格式化选项的打印接口
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };//创建并初始化printbuffer结构体

    if (prebuffer < 0)
    {
        return NULL;
    }

    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);//根据预分配大小调用hooks的allocate函数分配内存
    if (!p.buffer)
    {
        return NULL;
    }
    //初始化printbuffer
    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;//允许扩容，是与PrintPreallocated的核心区别
    p.format = fmt;
    p.hooks = global_hooks;

    if (!print_value(item, &p))//调用print_value函数递归遍历整个JSON树进行打印
    {
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    return (char*)p.buffer;
}
/*提供允许用户提供预分配缓冲区的打印接口，避免内部分配内存
用户负责提供缓冲区，指定确切大小，保证缓冲区在整个打印过程中有效*/
CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if ((length < 0) || (buffer == NULL))
    {
        return false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;//不允许扩容
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}

/* Parser core - when encountering text, process appropriately. */
/*解析器核心函数，根据输入文本的内容调用相应的解析函数来处理不同类型的JSON值
作为递归下降解析器的入口
parse_value->parse_string/parse_number/parse_object->递归调用parse_value*/
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false; /* no input */
    }

    /* parse the different types of values */
    /*分派类型：根据输入文本的内容调用相应的解析函数来处理不同类型的JSON值
     把null/false/true放在最前面
     减少不必要的字符判断*/

    /* null */
    /*can_read先检查边界，再比较内容
    首先检查输入缓冲区是否有足够的字符来匹配"null"
    当前字符序列与"null"相同设置cJSON节点的类型为cJSON_NULL
    偏移量增加4跳过"null"字符串 */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0))
    {
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return true;
    }
    /* false */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0))
    {
        item->type = cJSON_False;
        input_buffer->offset += 5;
        return true;
    }
    /* true */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0))//
    {
        item->type = cJSON_True;
        item->valueint = 1;//显式设置valueint
        input_buffer->offset += 4;
        return true;
    }
    /* string */
    /*遇到双引号调用parse_string函数解析字符串值
     parse_string函数会处理转义序列，生成解码后的字符串，并设置cJSON节点的类型和值字符串指针
     parse_value函数根据parse_string的结果返回true或false*/
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"'))
    {
        return parse_string(item, input_buffer);//内部更新offset
    }
    /* number */
    //遇到数字或负号调用parse_number函数解析数字值
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') || ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))))
    {
        return parse_number(item, input_buffer);
    }
    /* array */
    /*遇到左方括号调用parse_array函数解析数组值
     parse_array函数递归解析数组中的元素，并构建cJSON节点的子节点链表来表示数组结构*/
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '['))
    {
        return parse_array(item, input_buffer);
    }
    /* object */
    /*遇到左大括号，调用parse_object函数解析对象值
     parse_object函数递归解析对象中的键值对，并构建cJSON节点的子节点链表来表示对象结构*/
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{'))
    {
        return parse_object(item, input_buffer);
    }

    return false;
    /*失败路径的统一语义
    如果输入文本不匹配任何已知的JSON值类型
    返回false表示解析失败*/
}

/* Render a value to text. */
/*打印器核心：根据cJSON节点的类型调用相应的打印函数来生成JSON字符串
 print_value 是递归下降序列化的总入口：
 print_value-> print_string/print_number/print_object-> 递归调用print_value
 与 parse_value【0
 
 对称*/
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output = NULL;//输出缓冲区指针，指向生成的JSON字符串位置

    if ((item == NULL) || (output_buffer == NULL))
    {
        return false;
    }

    /*根据cJSON节点的类型调用相应的打印函数来生成JSON字符串
     把null/false/true放在最前面
     减少不必要的字符判断*/
     /*0xFF作为类型掩码，屏蔽高位只保留低8位类型
     只关心类型，不关心修饰标志
     引用/常量等不影响如何打印*/
    switch ((item->type) & 0xFF)
    {
        case cJSON_NULL:
            output = ensure(output_buffer, 5);//为null字符串分配空间
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "null");//写入null字符串到输出缓冲区
            return true;

        case cJSON_False:
            output = ensure(output_buffer, 6);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "false");
            return true;
            /*问题：ensure确保有空间，strcpy直接写入，那offset没更新啊？ */

        case cJSON_True:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "true");
            return true;

        case cJSON_Number://遇到数字类型，调用print_number函数生成数字的JSON字符串
            return print_number(item, output_buffer);

        case cJSON_Raw://遇到Raw类型，直接将valuestring的内容写入输出缓冲区，不进行转义或格式化
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                return false;
            }

            raw_length = strlen(item->valuestring) + sizeof("");
            output = ensure(output_buffer, raw_length);
            if (output == NULL)
            {
                return false;
            }
            memcpy(output, item->valuestring, raw_length);
            return true;
        }
         //复杂类型递归分派
        case cJSON_String://字符串类型调用print_string函数生成字符串的JSON表示
            return print_string(item, output_buffer);

        case cJSON_Array://数组类型调用print_array函数递归生成数组的JSON表示
            return print_array(item, output_buffer);
            /* 数组的打印内部递归调用print_value */

        case cJSON_Object://对象类型调用print_object函数递归生成对象的JSON表示
            return print_object(item, output_buffer);

        default:
            return false;
    }
}

/* Build an array from input text. */
//解析数组的函数，递归解析数组中的元素，并构建cJSON节点的子节点链表来表示数组结构
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;//链表的头结点
    /* head of the linked list */
    cJSON *current_item = NULL;//当前处理的数组元素节点指针

    if (input_buffer->depth >= CJSON_NESTING_LIMIT)//检查嵌套深度，防止过深嵌套导致栈溢出
    {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;//嵌套深度计数器

    if (buffer_at_offset(input_buffer)[0] != '[')//检查当前字符是否为左方括号'['
    {
        /* not an array */
        goto fail;
    }

    input_buffer->offset++;//跳过左方括号
    buffer_skip_whitespace(input_buffer);//跳过空白字符
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']'))//检查是否为右方括号']'，表示空数组
    {
        /* empty array */
        goto success;//空数组，直接跳转到success，跳过复杂的元素解析循环
    }

    /* check if we skipped to the end of the buffer */
    //检查是否已经到达输入缓冲区的末尾，如果是说明数组没有正确结束
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;
        goto fail;
    }

    /* step back to character in front of the first element */
    input_buffer->offset--;//回退一个字符，使偏移量停在第一个元素的前面以 进入元素解析循环
    /* loop through the comma separated array elements */
    //循环解析数组中的元素，直到遇到右方括号']'表示数组结束，进行双向链表的动态构建
    do
    {
        /* allocate next item */
        //为下一个数组元素分配一个新的cJSON节点，并传递hooks以使用自定义内存分配函数
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));//为下一个数组元素分配一个新的cJSON节点
        if (new_item == NULL)
        {
            goto fail; /* allocation failure */
        }

        /* attach next item to list */
        /*将新分配的cJSON节点连接到链表中
        如果是第一个元素，设置head和current_item指向它
        否则将其添加到链表末尾，并更新current_item指向新节点*/
        if (head == NULL)
        {
            /* start the linked list */
            //第一个元素，初始化链表的头结点和当前节点指针
            current_item = head = new_item;
        }
        else
        {
            /* add to the end and advance */
            //将新节点添加到链表末尾，更新当前节点的next指针和新节点的prev指针，将current_item指向新节点
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* parse next value */
        input_buffer->offset++;//跳过逗号或左方括号，解析下一个元素
        buffer_skip_whitespace(input_buffer);//跳过空白字符
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* failed to parse value */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || buffer_at_offset(input_buffer)[0] != ']')//检查是否为右方括号']'，如果不是说明数组没有正确结束
    {
        goto fail; /* expected end of array */
    }

success://解析成功
    input_buffer->depth--;

    if (head != NULL) {
        head->prev = current_item;//如果链表不为空，设置头结点的prev指针指向当前节点，形成双向链表
    }

    item->type = cJSON_Array;
    item->child = head;//设置cJSON节点的类型为cJSON_Array，子节点指向链表的头结点

    input_buffer->offset++;//跳过右方括号，完成数组的解析

    return true;

fail:
    if (head != NULL)//解析失败，释放已分配的链表节点
    {
        cJSON_Delete(head);
    }

    return false;
}

/* Render an array to text */
/*数组打印函数，递归生成数组的JSON表示
与parse_array的完美对称*/
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;//输出缓冲区指针，指向生成的JSON字符串位置
    size_t length = 0;
    cJSON *current_element = item->child;//获取数组的第一个元素

    if (output_buffer == NULL)
    {
        return false;
    }

    /* Compose the output array. */
    /* opening square bracket */
    output_pointer = ensure(output_buffer, 1);//为左方括号'['分配空间
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    while (current_element != NULL)//循环遍历数组中的元素，调用print_value递归生成每个元素的JSON表示，并将它们连接起来形成完整的数组字符串
    {
        if (!print_value(current_element, output_buffer))//为当前元素生成JSON字符串，失败返回false
        {
            return false;
        }
        update_offset(output_buffer);//更新输出缓冲区的偏移量，确保它反映了当前写入的位置

         /*如果当前元素不是数组中的最后一个元素，添加逗号分隔符
         如果format选项启用，还会添加一个空格，实现格式化输出*/
        if (current_element->next)
        {
            length = (size_t) (output_buffer->format ? 2 : 1);//计算逗号分隔符的长度，如果启用格式化输出长度为2
            output_pointer = ensure(output_buffer, length + 1);//为逗号分隔符分配空间
            if (output_pointer == NULL)
            {
                return false;
            }
            *output_pointer++ = ',';
            if(output_buffer->format)//如果启用格式化输出，添加一个空格
            {
                *output_pointer++ = ' ';
            }
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;//移动到下一个元素继续循环
    }

    output_pointer = ensure(output_buffer, 2);//为右方括号']'和字符串结束符分配空间
    if (output_pointer == NULL)
    {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;//完成数组的打印，更新嵌套深度

    return true;
}

/* Build an object from the text. */
/*解析对象的函数，递归解析对象中的键值对，并构建cJSON节点的子节点链表来表示对象结构
负责处理对象的嵌套结构，管理键名和值的配对关系，以及双向链表的构建。*/
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* linked list head */
    cJSON *current_item = NULL;

    if (input_buffer->depth >= CJSON_NESTING_LIMIT)//检查嵌套深度
    {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '{'))//检查当前字符是否为左大括号'{'
    {
        goto fail; /* not an object */
    }

    input_buffer->offset++;//跳过左大括号
    buffer_skip_whitespace(input_buffer);
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}'))//检查是否为右大括号'}'，表示空对象
    {
        goto success; /* empty object */
    }

    /* check if we skipped to the end of the buffer */
    if (cannot_access_at_index(input_buffer, 0))//检查是否已经到达末尾，如果是说明对象没有正确结束
    {
        input_buffer->offset--;
        goto fail;
    }

    /* step back to character in front of the first element */
    input_buffer->offset--;//回退一个字符，使偏移量停在第一个键的前面以进入键值对解析循环
    /* loop through the comma separated array elements */
    //循环解析对象中的键值对，直到遇到右大括号'}'表示对象结束，进行双向链表的动态构建
    do
    {
        /* allocate next item */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));//为下一个键值对分配一个新的cJSON节点
        if (new_item == NULL)
        {
            goto fail; /* allocation failure */
        }

        /* attach next item to list */
        //将新分配的cJSON节点连接到链表中
        if (head == NULL)
        {
            /* start the linked list */
            current_item = head = new_item;//第一个键值对，初始化链表的头结点和当前节点指针
        }
        else
        {
            /* add to the end and advance */
            current_item->next = new_item;//将新节点添加到链表末尾，更新当前节点的next指针和新节点的prev指针，将current_item指向新节点
            new_item->prev = current_item;
            current_item = new_item;
        }

        if (cannot_access_at_index(input_buffer, 1))//检查是否有足够的字符来解析键名和冒号
        {
            goto fail; /* nothing comes after the comma */
        }

        /* parse the name of the child */
        //解析键名，调用parse_string函数解析字符串值作为键名，并设置cJSON节点的string字段指向解析结果
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_string(current_item, input_buffer))
        {
            goto fail; /* failed to parse name */
        }
        buffer_skip_whitespace(input_buffer);

        /* swap valuestring and string, because we parsed the name */
        current_item->string = current_item->valuestring;//键名，存储在string字段中
        current_item->valuestring = NULL;//清空值

        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':'))//检查冒号分隔符，如果不是说明键值对没有正确格式化
        {
            goto fail; /* invalid object */
        }

        /* parse the value */
        //调用parse_value函数解析值
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* failed to parse value */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}'))
    {
        goto fail; /* expected end of object */
    }

success:
    input_buffer->depth--;//解析成功，更新嵌套深度

    if (head != NULL) {
        head->prev = current_item;//如果链表不为空，设置头结点的prev指针指向当前节点，形成双向链表
    }

    item->type = cJSON_Object;//设置节点类型
    item->child = head;//设置子节点指向链表的头结点

    input_buffer->offset++;//跳过右大括号，完成解析
    return true;

fail:
    if (head != NULL)//解析失败，释放已分配的链表节点
    {
        cJSON_Delete(head);
    }

    return false;
}

/* Render an object to text. */
/*对象打印函数，递归生成对象的JSON表示
与parse_object对称*/
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_item = item->child;//获取对象的第一个键值对节点

    if (output_buffer == NULL)
    {
        return false;
    }

    /* Compose the output: */
    /*对象的JSON表示以左大括号'{'开头，右大括号'}'结尾，键值对之间用逗号分隔
     每个键值对的键和值之间用冒号分隔
     如果启用格式化输出，还会在适当的位置添加换行符和缩进
     实现了格式化规则，并递归调用print_value生成每个键值对的JSON表示*/

     /* opening brace */
     //为左大括号'{'分配空间，长度根据是否启用格式化输出而定
    length = (size_t) (output_buffer->format ? 2 : 1); /* fmt: {\n */
    output_pointer = ensure(output_buffer, length + 1);//为左大括号'{'和可能的换行符分配空间
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;//增加嵌套深度，准备打印对象的内容
    if (output_buffer->format)//如果启用格式化输出，在左大括号后添加换行符
    {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;//更新输出缓冲区的偏移量，反映已写入的字符数

    while (current_item)//循环遍历对象中的键值对节点，生成每个键值对的JSON表示，并将它们连接起来形成完整的对象字符串
    {
        if (output_buffer->format)//如果启用格式化输出，在每个键值对前添加适当的缩进
        {
            size_t i;
            output_pointer = ensure(output_buffer, output_buffer->depth);//为缩进分配空间，长度根据当前嵌套深度而定
            if (output_pointer == NULL)
            {
                return false;
            }
            for (i = 0; i < output_buffer->depth; i++)//添加缩进
            {
                *output_pointer++ = '\t';
            }
            output_buffer->offset += output_buffer->depth;//更新偏移量，反映已写入的缩进字符数
        }

        /* print key */
        /*调用print_string_ptr函数生成键名的JSON表示，直接使用current_item->string作为输入
         print_string_ptr函数负责处理转义序列和格式化规则*/
        if (!print_string_ptr((unsigned char*)current_item->string, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);//更新输出缓冲区的偏移量,确保offset反映真实写入位置

        length = (size_t) (output_buffer->format ? 2 : 1);//计算键值分隔符的长度，如果启用格式化输出长度为2
        output_pointer = ensure(output_buffer, length);//分配空间
        if (output_pointer == NULL)
        {
            return false;
        }
        *output_pointer++ = ':';
        if (output_buffer->format)
        {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += length;

        /* print value */
        /*调用print_value函数生成值的JSON表示，递归处理值的类型和结构
         print_value函数会根据值的类型调用相应的打印函数来生成JSON字符串*/
        if (!print_value(current_item, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /* print comma if not last */
        /*如果当前键值对不是对象中的最后一个，添加逗号分隔符
         如果启用格式化输出，还会添加一个换行符*/
        length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
        output_pointer = ensure(output_buffer, length + 1);
        if (output_pointer == NULL)
        {
            return false;
        }
        if (current_item->next)
        {
            *output_pointer++ = ',';
        }

        if (output_buffer->format)
        {
            *output_pointer++ = '\n';
        }
        *output_pointer = '\0';
        output_buffer->offset += length;

        current_item = current_item->next;//移动到下一个键值对节点继续循环
    }
    
    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);//为右大括号'}'和换行符分配空间
    if (output_pointer == NULL)
    {
        return false;
    }
    if (output_buffer->format)//如果启用格式化输出，在右大括号前添加缩进
    {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++)
        {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;//完成对象的打印，更新嵌套深度

    return true;
}

/* Get Array size/item / object item. */
//提供访问数组和对象元素的接口函数，允许用户获取数组的大小、访问数组中的元素，以及根据键名访问对象中的元素
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;//数组元素的临时指针
    size_t size = 0;

    if (array == NULL)
    {
        return 0;
    }

    child = array->child;//获取数组的第一个元素

    while(child != NULL)
    {
        size++;
        child = child->next;//遍历数组中的元素，统计元素的数量，直到链表末尾
    }

    /* FIXME: Can overflow here. Cannot be fixed without breaking the API */
     /*统计完成后返回数组的大小，注意size_t类型可能会溢出，但由于API设计限制无法修复这个问题
     需要用户注意不要创建过大的数组以避免溢出*/
    return (int)size;
}
//根据索引获取数组中的元素，先检查输入是否有效，然后遍历数组的子节点链表，直到找到指定索引的元素或到达链表末尾
static cJSON* get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;//获取数组的第一个元素
    while ((current_child != NULL) && (index > 0))//遍历数组中的元素，直到找到指定索引的元素或到达链表末尾
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;//返回找到的元素
}

//根据索引获取数组中的元素的公共接口函数,调用get_array_item函数进行元素访问
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0)
    {
        return NULL;
    }

    return get_array_item(array, (size_t)index);
}

//根据键名获取对象中的元素，遍历对象的子节点链表，比较每个节点的string字段与指定的键名，直到找到匹配的元素或到达链表末尾
static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;//获取对象的第一个键值对节点
    if (case_sensitive)//根据case_sensitive参数决定使用区分大小写的字符串比较函数strcmp还是不区分大小写的函数case_insensitive_strcmp
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;//遍历对象中的键值对节点并比较
        }
    }
    else
    {
        while ((current_element != NULL) && (case_insensitive_strcmp((const unsigned char*)name, (const unsigned char*)(current_element->string)) != 0))
        {
            current_element = current_element->next;
        }
    }

    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;//返回找到的元素
}

//根据键名获取对象中的元素的公共接口函数，调用get_object_item函数进行元素访问
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, false);
}
//根据键名获取对象中的元素的公共接口函数，调用get_object_item函数进行元素访问，区分大小写
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

//检查对象中是否存在指定键名的元素，调用cJSON_GetObjectItem函数获取元素，返回非NULL表示存在，返回1，否则返回0
CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Utility for array list handling. */
//辅助函数suffix_object，用于将一个cJSON节点连接到另一个节点的后面，形成双向链表结构
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* Utility for handling references. */
//创建一个cJSON节点作为输入节点的引用
static cJSON *create_reference(const cJSON *item, const internal_hooks * const hooks)
{
    cJSON *reference = NULL;
    if (item == NULL)
    {
        return NULL;
    }

    reference = cJSON_New_Item(hooks);//为引用节点分配内存
    if (reference == NULL)
    {
        return NULL;
    }

    memcpy(reference, item, sizeof(cJSON));//将输入节点的内容复制到引用节点中，形成一个新的cJSON节点，具有相同的类型和值
    reference->string = NULL;//引用节点不需要字符串字段，设置为NULL
    reference->type |= cJSON_IsReference;//设置类型标志，表示这是一个引用节点
    reference->next = reference->prev = NULL;//引用节点不参与链表结构
    return reference;
}
//将一个cJSON节点添加到一个数组节点的末尾，形成双向链表结构
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;
    /*
     * To find the last item in array quickly, we use prev in array
     */
    //如果数组已经有元素，使用数组节点的prev指针直接访问最后一个元素，避免遍历整个链表，提高效率
    if (child == NULL)
    {
        /* list is empty, start new one */
        //数组为空，直接将新节点作为第一个元素，设置数组的child指针指向它，并初始化prev和next指针
        array->child = item;
        item->prev = item;
        item->next = NULL;
    }
    else
    {
        /* append to the end */
        //数组不为空，将新节点添加到链表末尾，更新最后一个元素的next指针和新节点的prev指针，并将数组的prev指针指向新节点
        if (child->prev)
        {
            suffix_object(child->prev, item);
            array->child->prev = item;
        }
    }

    return true;
}

/* Add item to array/object. */
//添加数组节点的公共接口函数，调用add_item_to_array函数进行添加
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* helper function to cast away const */
static void* cast_away_const(const void* string)
{
    return (void*)string;
}
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic pop
#endif

//向cJSON对象中添加新节点。指定键名，并根据constant_key参数决定是否将键名作为常量字符串处理
static cJSON_bool add_item_to_object(cJSON * const object, const char * const string, cJSON * const item, const internal_hooks * const hooks, const cJSON_bool constant_key)
{
    char *new_key = NULL;//新键名的指针
    int new_type = cJSON_Invalid;//新节点的类型

    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item))
    {
        return false;
    }

    if (constant_key)//如果constant_key为true，直接使用输入的字符串作为键名，并设置类型为常量字符串
    {
        new_key = (char*)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    }
    else
    {
        new_key = (char*)cJSON_strdup((const unsigned char*)string, hooks);//如果constant_key为false，调用cJSON_strdup函数复制输入字符串，设置类型为普通字符串
        if (new_key == NULL)
        {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))//如果节点类型不是常量字符串，并且已经有键名，释放原有的键名字符串
    {
        hooks->deallocate(item->string);
    }

    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}

//向cJSON对象中添加新节点的公共接口函数，调用add_item_to_object函数进行添加，constant_key参数为false表示键名不是常量字符串
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/* Add an item to an object with constant string as key */
//向cJSON对象中添加新节点的公共接口函数，调用add_item_to_object函数进行添加，constant_key参数为true表示键名是常量字符串
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}
//向数组中添加一个节点的引用，调用create_reference函数创建一个引用节点，并将其添加到数组中
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL)
    {
        return false;
    }

    return add_item_to_array(array, create_reference(item, &global_hooks));
}

//向对象中添加一个节点的引用，调用create_reference函数创建一个引用节点，并将其添加到对象中，constant_key参数为false表示键名不是常量字符串
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((object == NULL) || (string == NULL))
    {
        return false;
    }

    return add_item_to_object(object, string, create_reference(item, &global_hooks), &global_hooks, false);
}

CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name)//向对象中添加一个值为null的节点，调用cJSON_CreateNull函数创建一个null节点，并将其添加到对象中
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false))
    {
        return null;
    }

    cJSON_Delete(null);//添加失败，释放创建的null节点
    return NULL;
}

//向对象中添加一个值为true的节点，调用cJSON_CreateTrue函数创建一个true节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false))
    {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}
//向对象中添加一个值为false的节点，调用cJSON_CreateFalse函数创建一个false节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false))
    {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}
//向对象中添加一个布尔值节点，调用cJSON_CreateBool函数创建一个布尔值节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false))
    {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}
//向对象中添加一个数值节点，调用cJSON_CreateNumber函数创建一个数值节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false))
    {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}
//向对象中添加一个字符串节点，调用cJSON_CreateString函数创建一个字符串节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false))
    {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}
//向对象中添加一个Raw节点，调用cJSON_CreateRaw函数创建一个Raw节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false))
    {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}
//向对象中添加一个对象节点，调用cJSON_CreateObject函数创建一个对象节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false))
    {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}
//向对象中添加一个数组节点，调用cJSON_CreateArray函数创建一个数组节点，并将其添加到对象中
CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false))
    {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

//提供从数组或对象中分离一个节点的接口函数
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))//检查父节点和要分离的节点都不为空，并且要分离的节点是父节点的子节点
    {
        return NULL;
    }

    if (item != parent->child)//如果要分离的节点不是父节点的第一个子节点，更新前一个节点的next指针跳过当前节点
    {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* not the last element */
        item->next->prev = item->prev;//如果要分离的节点不是父节点的最后一个子节点，更新下一个节点的prev指针跳过当前节点
    }

    if (item == parent->child)//如果要分离的节点是父节点的第一个子节点，更新父节点的child指针指向下一个节点
    {
        /* first element */
        parent->child = item->next;
    }
    else if (item->next == NULL)//如果要分离的节点是父节点的最后一个子节点，更新父节点的child的prev指针指向前一个节点
    {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;//清空要分离的节点的prev和next指针
    item->next = NULL;

    return item;
}
//根据索引从数组中分离一个节点，调用cJSON_DetachItemViaPointer函数进行分离
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
    //根据索引从数组中删除一个节点，先调用cJSON_DetachItemFromArray函数分离节点，然后调用cJSON_Delete函数释放分离的节点
}
//根据键名从对象中分离一个节点，调用cJSON_DetachItemViaPointer函数进行分离
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}
//根据键名从对象中分离一个节点，区分大小写
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}
//根据键名从对象中删除一个节点，先调用cJSON_DetachItemFromObject函数分离节点，然后调用cJSON_Delete函数释放分离的节点
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}
 //根据键名从对象中删除一个节点，区分大小写
CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Replace array/object items with new ones. */
//提供替换数组或对象中节点的接口函数，根据索引或键名将一个新的cJSON节点替换掉原有的节点
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;//要插入位置的后一个节点的指针

    if (which < 0 || newitem == NULL)
    {
        return false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    //获取要插入位置的后一个节点，如果which等于数组的大小，after_inserted将为NULL，表示在末尾插入
    if (after_inserted == NULL)
    {
        return add_item_to_array(array, newitem);
    }

    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* return false if after_inserted is a corrupted array item */
        //如果要插入位置的后一个节点不是数组的第一个子节点，并且它的prev指针为NULL，说明数组结构损坏
        return false;
    }

    newitem->next = after_inserted;//将新节点的next指针指向要插入位置的后一个节点
    newitem->prev = after_inserted->prev;//将新节点的prev指针指向要插入位置的前一个节点
    after_inserted->prev = newitem;//将要插入位置的后一个节点的prev指针指向新节点
    if (after_inserted == array->child)//如果要插入位置的后一个节点是数组的第一个子节点，更新数组的child指针指向新节点
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
    return true;
}


CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
//根据指针将一个新的cJSON节点替换掉原有的节点，更新链表结构，并删除原有的节点
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL))
    {
        return false;
    }

    if (replacement == item)//如果要替换的节点和新节点是同一个节点，直接返回true
    {
        return true;
    }

    replacement->next = item->next;//将新节点的next指针指向原有节点的下一个节点
    replacement->prev = item->prev;//将新节点的prev指针指向原有节点的前一个节点

    if (replacement->next != NULL)//如果新节点的下一个节点不为NULL，更新下一个节点的prev指针指向新节点
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)//如果要替换的节点是父节点的第一个子节点，更新父节点的child指针指向新节点
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        /*如果要替换的节点不是父节点的第一个子节点，更新前一个节点的next指针指向新节点
        并且如果要替换的节点是父节点的最后一个子节点
        更新父节点的child的prev指针指向新节点*/
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;//将原有节点的next和prev指针清空
    item->prev = NULL;
    cJSON_Delete(item);//删除原有节点

    return true;
}


CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

//根据键名将一个新的cJSON节点替换掉对象中原有的节点，调用cJSON_ReplaceItemViaPointer函数进行替换，case_sensitive参数决定是否区分大小写
static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    if ((replacement == NULL) || (string == NULL))
    {
        return false;
    }

    /* replace the name in the replacement */
    /*在替换节点中设置正确的键名
     调用cJSON_ReplaceItemViaPointer函数进行替换
     如果替换节点原来有一个非空的键名，先释放原有的键名字符串*/
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL))
    {
        cJSON_free(replacement->string);//释放原有的键名字符串
    }
    replacement->string = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);//复制输入的键名字符串，设置到替换节点中
    if (replacement->string == NULL)
    {
        return false;
    }

    replacement->type &= ~cJSON_StringIsConst;//替换节点的类型标志中不包含cJSON_StringIsConst，表示键名不是常量字符串

    return cJSON_ReplaceItemViaPointer(object, get_object_item(object, string, case_sensitive), replacement);
}
 //替换节点，调用replace_item_in_object函数进行替换，不区分大小写
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}
//替换节点，区分大小写
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/* Create basic types: */
//提供创建基本类型节点的接口函数，包括null、true、false、布尔值、数值、字符串、Raw、数组和对象等类型，调用cJSON_New_Item函数分配内存，并设置相应的类型
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;//根据输入的布尔值设置节点类型为cJSON_True或cJSON_False
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Number;//设置节点类型为cJSON_Number
        item->valuedouble = num;//将输入的数值存储在valuedouble字段中

        /* use saturation in case of overflow */
        //处理数值溢出
        if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL)
    {
        item->type = cJSON_String | cJSON_IsReference;//设置节点类型为字符串，并且标记为引用类型
        item->valuestring = (char*)cast_away_const(string);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type=cJSON_Array;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item)
    {
        item->type = cJSON_Object;
    }

    return item;
}

/* Create Arrays: */
//创建数组节点，并将相应的元素添加到数组中
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;//当前元素的指针
    cJSON *p = NULL;//前一个元素的指针
    cJSON *a = NULL;//数组节点的指针

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();//创建一个数组节点

    for(i = 0; a && (i < (size_t)count); i++)//遍历输入的整数数组，为每个元素创建一个数值节点，并将其添加到数组中
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;//如果是第一个元素，直接将其设置为数组的child
        }
        else
        {
            suffix_object(p, n);//如果不是第一个元素，调用suffix_object函数将其连接到前一个元素的后面
        }
        p = n;//更新前一个元素的指针
    }

    if (a && a->child) {
        a->child->prev = n;//设置数组的child的prev指针指向最后一个元素，形成双向链表结构
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
//创建一个浮点数数组节点
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
//创建双精度浮点数数组节点
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}
//创建字符串数组节点
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* Duplication */
//提供复制cJSON节点的接口函数，调用cJSON_Duplicate_rec函数进行递归复制
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    return cJSON_Duplicate_rec(item, 0, recurse );
}
//递归复制cJSON节点的内部函数，创建一个新的节点，如果recurse参数为true，递归复制子节点
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;//新节点的指针
    cJSON *child = NULL;//当前子节点的指针
    cJSON *next = NULL;//前一个子节点的指针
    cJSON *newchild = NULL;//复制的子节点的指针

    /* Bail on bad ptr */
    if (!item)
    {
        goto fail;
    }
    /* Create new item */
    //调用cJSON_New_Item函数分配内存创建一个新的cJSON节点
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem)
    {
        goto fail;
    }
    /* Copy over all vars */
    //复制原有节点的信息到新节点中
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring)//如果原有节点是字符串类型，复制字符串内容到新节点中
    {
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring, &global_hooks);
        if (!newitem->valuestring)
        {
            goto fail;
        }
    }
    if (item->string)//如果原有节点有键名，复制键名字符串到新节点中
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string, &global_hooks);
        //键名是常量字符串，直接使用原有的键名，否则调用cJSON_strdup函数复制键名字符串
        if (!newitem->string)
        {
            goto fail;
        }
    }
    /* If non-recursive, then we're done! */
    //如果recurse参数为false，表示不需要递归复制子节点，直接返回新节点
    if (!recurse)
    {
        return newitem;
    }
    /* Walk the ->next chain for the child. */
    //需要递归复制子节点
    child = item->child;//获取原有节点的第一个子节点
    while (child != NULL)//遍历原有节点的子节点链表
    {
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }
        newchild = cJSON_Duplicate_rec(child, depth + 1, true); /* Duplicate (with recurse) each item in the ->next chain */
        //递归复制当前子节点，深度加1
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /* If newitem->child already set, then crosswire ->prev and ->next and move on */
            //如果已经有一个子节点 ，将新复制的子节点连接到前一个子节点的后面，并更新前一个子节点的指针
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* Set newitem->child and move to it */
            //如果还没有子节点，将新复制的子节点设置为新节点的child，并更新前一个子节点的指针
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
    if (newitem && newitem->child)
    {
        newitem->child->prev = newchild;//设置新节点的child的prev指针指向最后一个复制的子节点，形成双向链表结构
    }

    return newitem;

fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem);//复制错误释放已经创建的新节点
    }

    return NULL;
}

//提供压缩JSON字符串的接口函数
static void skip_oneline_comment(char **input)//跳过单行注释
{
    *input += static_strlen("//");//跳过单行注释的开头

    for (; (*input)[0] != '\0'; ++(*input))//遍历输入字符串，直到遇到换行符或字符串结束
    {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

static void skip_multiline_comment(char **input)//跳过多行注释
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if (((*input)[0] == '*') && ((*input)[1] == '/'))
        {
            *input += static_strlen("*/");
            return;
        }
    }
}

static void minify_string(char **input, char **output) 
//压缩字符串，保持内容不变，去掉字符串外的引号和转义字符
{
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}


CJSON_PUBLIC(void) cJSON_Minify(char *json)//去掉字符串中的空白字符和注释
 {
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (json[0] != '\0')//遍历输入字符串，根据不同的字符类型进行处理
    {
        switch (json[0])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                json++;
                break;

            case '/':
                if (json[1] == '/')//如果遇到单行注释的开头，调用skip_oneline_comment函数跳过单行注释
                {
                    skip_oneline_comment(&json);
                }
                else if (json[1] == '*')//如果遇到多行注释的开头，调用skip_multiline_comment函数跳过多行注释
                {
                    skip_multiline_comment(&json);
                } else {
                    json++;
                }
                break;

            case '\"':
                minify_string(&json, (char**)&into);//如果遇到字符串的开头，调用minify_string函数压缩字符串
                break;

            default:
                into[0] = json[0];
                json++;
                into++;
        }
    }

    /* and null-terminate. */
    //添加一个字符串结束符
    *into = '\0';
}

//检查cJSON节点类型的接口函数，根据节点的type字段判断节点的类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}
//检查cJSON节点是否为false类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}
//检查cJSON节点是否为true类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}

//检查cJSON节点是否为布尔类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}
//检查cJSON节点是否为数值类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}
//检查cJSON节点是否为字符串类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}
//检查cJSON节点是否为数组类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}
//检查cJSON节点是否为对象类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}
//检查cJSON节点是否为Raw类型
CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}
//比较两个cJSON节点是否相等，首先检查节点类型是否相同，然后根据不同的类型进行比较
CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))//检查两个节点都不为NULL，并且类型相同
    {
        return false;
    }

    /* check if type is valid */
    //检查节点类型是否有效
    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Number:
        case cJSON_String:
        case cJSON_Raw:
        case cJSON_Array:
        case cJSON_Object:
            break;

        default:
            return false;
    }

    /* identical objects are equal */
    //两个节点是同一个，直接返回true
    if (a == b)
    {
        return true;
    }

    switch (a->type & 0xFF)//根据不同的类型进行比较
    {
        /* in these cases and equal type is enough */
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return true;

        case cJSON_Number:
            if (compare_double(a->valuedouble, b->valuedouble))//比较数值类型节点的数值是否相等
            {
                return true;
            }
            return false;

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return false;
            }
            if (strcmp(a->valuestring, b->valuestring) == 0)
            {
                return true;
            }

            return false;

        case cJSON_Array:
        /*比较数组类型节点的元素是否相等
        获取两个数组的第一个子节点，然后依次比较每个子节点
        有任何一个子节点不相等，或者两个数组的长度不同，则数组不相等*/
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            for (; (a_element != NULL) && (b_element != NULL);)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }

                a_element = a_element->next;//获取下一个子节点
                b_element = b_element->next;
            }

            /* one of the arrays is longer than the other */
            if (a_element != b_element) {
                return false;
            }

            return true;
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a)//遍历对象a的子节点，对于每个子节点，在对象b中查找具有相同键名的子节点，并比较它们是否相等
            {
                /* TODO This has O(n^2) runtime, which is horrible! */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }

            /* doing this twice, once on a and b to prevent true comparison if a subset of b
             * TODO: Do this the proper way, this is just a fix for now */
            cJSON_ArrayForEach(b_element, b)
            //遍历对象b的子节点，对于每个子节点，在对象a中查找具有相同键名的子节点，比较它们是否相等，防止对象a是对象b的子集
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
    }
}
//调用全局的内存分配和释放函数指针进行操作
CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_hooks.allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
    object = NULL;
}
