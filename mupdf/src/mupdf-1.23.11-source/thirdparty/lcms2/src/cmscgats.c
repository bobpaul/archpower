//---------------------------------------------------------------------------------
//
//  Little Color Management System
//  Copyright (c) 1998-2022 Marti Maria Saguer
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//---------------------------------------------------------------------------------
//

#include "lcms2_internal.h"


// IT8.7 / CGATS.17-200x handling -----------------------------------------------------------------------------


#define MAXID        128     // Max length of identifier
#define MAXSTR      1024     // Max length of string
#define MAXTABLES    255     // Max Number of tables in a single stream
#define MAXINCLUDE    20     // Max number of nested includes

#define DEFAULT_DBL_FORMAT  "%.10g" // Double formatting

#ifdef CMS_IS_WINDOWS_
#    include <io.h>
#    define DIR_CHAR    '\\'
#else
#    define DIR_CHAR    '/'
#endif


// Symbols
typedef enum {

        SUNDEFINED,
        SINUM,      // Integer
        SDNUM,      // Real
        SIDENT,     // Identifier
        SSTRING,    // string
        SCOMMENT,   // comment
        SEOLN,      // End of line
        SEOF,       // End of stream
        SSYNERROR,  // Syntax error found on stream

        // Keywords

        SBEGIN_DATA,
        SBEGIN_DATA_FORMAT,
        SEND_DATA,
        SEND_DATA_FORMAT,
        SKEYWORD,
        SDATA_FORMAT_ID,
        SINCLUDE

    } SYMBOL;


// How to write the value
typedef enum {

        WRITE_UNCOOKED,
        WRITE_STRINGIFY,
        WRITE_HEXADECIMAL,
        WRITE_BINARY,
        WRITE_PAIR

    } WRITEMODE;

// Linked list of variable names
typedef struct _KeyVal {

        struct _KeyVal*  Next;
        char*            Keyword;       // Name of variable
        struct _KeyVal*  NextSubkey;    // If key is a dictionary, points to the next item
        char*            Subkey;        // If key is a dictionary, points to the subkey name
        char*            Value;         // Points to value
        WRITEMODE        WriteAs;       // How to write the value

   } KEYVALUE;


// Linked list of memory chunks (Memory sink)
typedef struct _OwnedMem {

        struct _OwnedMem* Next;
        void *            Ptr;          // Point to value

   } OWNEDMEM;

// Suballocator
typedef struct _SubAllocator {

         cmsUInt8Number* Block;
         cmsUInt32Number BlockSize;
         cmsUInt32Number Used;

    } SUBALLOCATOR;

// Table. Each individual table can hold properties and rows & cols
typedef struct _Table {

        char SheetType[MAXSTR];               // The first row of the IT8 (the type)

        int            nSamples, nPatches;    // Cols, Rows
        int            SampleID;              // Pos of ID

        KEYVALUE*      HeaderList;            // The properties

        char**         DataFormat;            // The binary stream descriptor
        char**         Data;                  // The binary stream

    } TABLE;

// File stream being parsed
typedef struct _FileContext {
        char           FileName[cmsMAX_PATH];    // File name if being read from file
        FILE*          Stream;                   // File stream or NULL if holded in memory
    } FILECTX;

//Very simple string
typedef struct {

        struct struct_it8* it8;
        cmsInt32Number max;
        cmsInt32Number len;
        char* begin;
    } string;


// This struct hold all information about an open IT8 handler.
typedef struct struct_it8 {

        cmsUInt32Number  TablesCount;                     // How many tables in this stream
        cmsUInt32Number  nTable;                          // The actual table

        TABLE Tab[MAXTABLES];

        // Memory management
        OWNEDMEM*      MemorySink;            // The storage backend
        SUBALLOCATOR   Allocator;             // String suballocator -- just to keep it fast

        // Parser state machine
        SYMBOL             sy;                // Current symbol
        int                ch;                // Current character

        cmsInt32Number     inum;              // integer value
        cmsFloat64Number   dnum;              // real value

        string*        id;            // identifier
        string*        str;           // string

        // Allowed keywords & datasets. They have visibility on whole stream
        KEYVALUE*      ValidKeywords;
        KEYVALUE*      ValidSampleID;

        char*          Source;                // Points to loc. being parsed
        cmsInt32Number lineno;                // line counter for error reporting

        FILECTX*       FileStack[MAXINCLUDE]; // Stack of files being parsed
        cmsInt32Number IncludeSP;             // Include Stack Pointer

        char*          MemoryBlock;           // The stream if holded in memory

        char           DoubleFormatter[MAXID];// Printf-like 'cmsFloat64Number' formatter

   } cmsIT8;


// The stream for save operations
typedef struct {

        FILE* stream;   // For save-to-file behaviour

        cmsUInt8Number* Base;
        cmsUInt8Number* Ptr;        // For save-to-mem behaviour
        cmsUInt32Number Used;
        cmsUInt32Number Max;

    } SAVESTREAM;


// ------------------------------------------------------ cmsIT8 parsing routines


// A keyword
typedef struct {

        const char *id;
        SYMBOL sy;

   } KEYWORD;

// The keyword->symbol translation table. Sorting is required.
static const KEYWORD TabKeys[] = {

        {"$INCLUDE",               SINCLUDE},   // This is an extension!
        {".INCLUDE",               SINCLUDE},   // This is an extension!

        {"BEGIN_DATA",             SBEGIN_DATA },
        {"BEGIN_DATA_FORMAT",      SBEGIN_DATA_FORMAT },
        {"DATA_FORMAT_IDENTIFIER", SDATA_FORMAT_ID},
        {"END_DATA",               SEND_DATA},
        {"END_DATA_FORMAT",        SEND_DATA_FORMAT},
        {"KEYWORD",                SKEYWORD}
        };

#define NUMKEYS (sizeof(TabKeys)/sizeof(KEYWORD))

// Predefined properties

// A property
typedef struct {
        const char *id;    // The identifier
        WRITEMODE as;      // How is supposed to be written
    } PROPERTY;

static PROPERTY PredefinedProperties[] = {

        {"NUMBER_OF_FIELDS", WRITE_UNCOOKED},    // Required - NUMBER OF FIELDS
        {"NUMBER_OF_SETS",   WRITE_UNCOOKED},    // Required - NUMBER OF SETS
        {"ORIGINATOR",       WRITE_STRINGIFY},   // Required - Identifies the specific system, organization or individual that created the data file.
        {"FILE_DESCRIPTOR",  WRITE_STRINGIFY},   // Required - Describes the purpose or contents of the data file.
        {"CREATED",          WRITE_STRINGIFY},   // Required - Indicates date of creation of the data file.
        {"DESCRIPTOR",       WRITE_STRINGIFY},   // Required  - Describes the purpose or contents of the data file.
        {"DIFFUSE_GEOMETRY", WRITE_STRINGIFY},   // The diffuse geometry used. Allowed values are "sphere" or "opal".
        {"MANUFACTURER",     WRITE_STRINGIFY},
        {"MANUFACTURE",      WRITE_STRINGIFY},   // Some broken Fuji targets does store this value
        {"PROD_DATE",        WRITE_STRINGIFY},   // Identifies year and month of production of the target in the form yyyy:mm.
        {"SERIAL",           WRITE_STRINGIFY},   // Uniquely identifies individual physical target.

        {"MATERIAL",         WRITE_STRINGIFY},    // Identifies the material on which the target was produced using a code
                                                  // uniquely identifying th e material. This is intend ed to be used for IT8.7
                                                  // physical targets only (i.e . IT8.7/1 and IT8.7/2).

        {"INSTRUMENTATION",  WRITE_STRINGIFY},    // Used to report the specific instrumentation used (manufacturer and
                                                  // model number) to generate the data reported. This data will often
                                                  // provide more information about the particular data collected than an
                                                  // extensive list of specific details. This is particularly important for
                                                  // spectral data or data derived from spectrophotometry.

        {"MEASUREMENT_SOURCE", WRITE_STRINGIFY},  // Illumination used for spectral measurements. This data helps provide
                                                  // a guide to the potential for issues of paper fluorescence, etc.

        {"PRINT_CONDITIONS", WRITE_STRINGIFY},     // Used to define the characteristics of the printed sheet being reported.
                                                   // Where standard conditions have been defined (e.g., SWOP at nominal)
                                                   // named conditions may suffice. Otherwise, detailed information is
                                                   // needed.

        {"SAMPLE_BACKING",   WRITE_STRINGIFY},     // Identifies the backing material used behind the sample during
                                                   // measurement. Allowed values are "black", "white", or {"na".

        {"CHISQ_DOF",        WRITE_STRINGIFY},     // Degrees of freedom associated with the Chi squared statistic
                                                   // below properties are new in recent specs:

        {"MEASUREMENT_GEOMETRY", WRITE_STRINGIFY}, // The type of measurement, either reflection or transmission, should be indicated
                                                   // along with details of the geometry and the aperture size and shape. For example,
                                                   // for transmission measurements it is important to identify 0/diffuse, diffuse/0,
                                                   // opal or integrating sphere, etc. For reflection it is important to identify 0/45,
                                                   // 45/0, sphere (specular included or excluded), etc.

       {"FILTER",            WRITE_STRINGIFY},     // Identifies the use of physical filter(s) during measurement. Typically used to
                                                   // denote the use of filters such as none, D65, Red, Green or Blue.

       {"POLARIZATION",      WRITE_STRINGIFY},     // Identifies the use of a physical polarization filter during measurement. Allowed
                                                   // values are {"yes", "white", "none" or "na".

       {"WEIGHTING_FUNCTION", WRITE_PAIR},         // Indicates such functions as: the CIE standard observer functions used in the
                                                   // calculation of various data parameters (2 degree and 10 degree), CIE standard
                                                   // illuminant functions used in the calculation of various data parameters (e.g., D50,
                                                   // D65, etc.), density status response, etc. If used there shall be at least one
                                                   // name-value pair following the WEIGHTING_FUNCTION tag/keyword. The first attribute
                                                   // in the set shall be {"name" and shall identify the particular parameter used.
                                                   // The second shall be {"value" and shall provide the value associated with that name.
                                                   // For ASCII data, a string containing the Name and Value attribute pairs shall follow
                                                   // the weighting function keyword. A semi-colon separates attribute pairs from each
                                                   // other and within the attribute the name and value are separated by a comma.

       {"COMPUTATIONAL_PARAMETER", WRITE_PAIR},    // Parameter that is used in computing a value from measured data. Name is the name
                                                   // of the calculation, parameter is the name of the parameter used in the calculation
                                                   // and value is the value of the parameter.

       {"TARGET_TYPE",        WRITE_STRINGIFY},    // The type of target being measured, e.g. IT8.7/1, IT8.7/3, user defined, etc.

       {"COLORANT",           WRITE_STRINGIFY},    // Identifies the colorant(s) used in creating the target.

       {"TABLE_DESCRIPTOR",   WRITE_STRINGIFY},    // Describes the purpose or contents of a data table.

       {"TABLE_NAME",         WRITE_STRINGIFY}     // Provides a short name for a data table.
};

#define NUMPREDEFINEDPROPS (sizeof(PredefinedProperties)/sizeof(PROPERTY))


// Predefined sample types on dataset
static const char* PredefinedSampleID[] = {
        "SAMPLE_ID",      // Identifies sample that data represents
        "STRING",         // Identifies label, or other non-machine readable value.
                          // Value must begin and end with a " symbol

        "CMYK_C",         // Cyan component of CMYK data expressed as a percentage
        "CMYK_M",         // Magenta component of CMYK data expressed as a percentage
        "CMYK_Y",         // Yellow component of CMYK data expressed as a percentage
        "CMYK_K",         // Black component of CMYK data expressed as a percentage
        "D_RED",          // Red filter density
        "D_GREEN",        // Green filter density
        "D_BLUE",         // Blue filter density
        "D_VIS",          // Visual filter density
        "D_MAJOR_FILTER", // Major filter d ensity
        "RGB_R",          // Red component of RGB data
        "RGB_G",          // Green component of RGB data
        "RGB_B",          // Blue com ponent of RGB data
        "SPECTRAL_NM",    // Wavelength of measurement expressed in nanometers
        "SPECTRAL_PCT",   // Percentage reflectance/transmittance
        "SPECTRAL_DEC",   // Reflectance/transmittance
        "XYZ_X",          // X component of tristimulus data
        "XYZ_Y",          // Y component of tristimulus data
        "XYZ_Z",          // Z component of tristimulus data
        "XYY_X",          // x component of chromaticity data
        "XYY_Y",          // y component of chromaticity data
        "XYY_CAPY",       // Y component of tristimulus data
        "LAB_L",          // L* component of Lab data
        "LAB_A",          // a* component of Lab data
        "LAB_B",          // b* component of Lab data
        "LAB_C",          // C*ab component of Lab data
        "LAB_H",          // hab component of Lab data
        "LAB_DE",         // CIE dE
        "LAB_DE_94",      // CIE dE using CIE 94
        "LAB_DE_CMC",     // dE using CMC
        "LAB_DE_2000",    // CIE dE using CIE DE 2000
        "MEAN_DE",        // Mean Delta E (LAB_DE) of samples compared to batch average
                          // (Used for data files for ANSI IT8.7/1 and IT8.7/2 targets)
        "STDEV_X",        // Standard deviation of X (tristimulus data)
        "STDEV_Y",        // Standard deviation of Y (tristimulus data)
        "STDEV_Z",        // Standard deviation of Z (tristimulus data)
        "STDEV_L",        // Standard deviation of L*
        "STDEV_A",        // Standard deviation of a*
        "STDEV_B",        // Standard deviation of b*
        "STDEV_DE",       // Standard deviation of CIE dE
        "CHI_SQD_PAR"};   // The average of the standard deviations of L*, a* and b*. It is
                          // used to derive an estimate of the chi-squared parameter which is
                          // recommended as the predictor of the variability of dE

#define NUMPREDEFINEDSAMPLEID (sizeof(PredefinedSampleID)/sizeof(char *))

//Forward declaration of some internal functions
static void* AllocChunk(cmsContext ContextID, cmsIT8* it8, cmsUInt32Number size);

static
string* StringAlloc(cmsContext ContextID, cmsIT8* it8, int max)
{
    string* s = (string*) AllocChunk(ContextID, it8, sizeof(string));
    if (s == NULL) return NULL;

    s->it8 = it8;
    s->max = max;
    s->len = 0;
    s->begin = (char*) AllocChunk(ContextID, it8, s->max);

    return s;
}

static
void StringClear(string* s)
{
    s->len = 0;
}

static
void StringAppend(cmsContext ContextID, string* s, char c)
{
    if (s->len + 1 >= s->max)
    {
        char* new_ptr;

        s->max *= 10;
        new_ptr = (char*) AllocChunk(ContextID, s->it8, s->max);
        if (new_ptr != NULL && s->begin != NULL)
            memcpy(new_ptr, s->begin, s->len);

        s->begin = new_ptr;
    }

    if (s->begin != NULL)
    {
        s->begin[s->len++] = c;
        s->begin[s->len] = 0;
    }
}

static
char* StringPtr(string* s)
{
    return s->begin;
}

static
void StringCat(cmsContext ContextID, string* s, const char* c)
{
    while (*c)
    {
        StringAppend(ContextID, s, *c);
        c++;
    }
}


// Checks whatever c is a separator
static
cmsBool isseparator(int c)
{
    return (c == ' ') || (c == '\t') ;
}

// Checks whatever c is a valid identifier char
static
cmsBool ismiddle(int c)
{
   return (!isseparator(c) && (c != '#') && (c !='\"') && (c != '\'') && (c > 32) && (c < 127));
}

// Checks whatsever c is a valid identifier middle char.
static
cmsBool isidchar(int c)
{
   return isalnum(c) || ismiddle(c);
}

// Checks whatsever c is a valid identifier first char.
static
cmsBool isfirstidchar(int c)
{
     return !isdigit(c) && ismiddle(c);
}

// Guess whether the supplied path looks like an absolute path
static
cmsBool isabsolutepath(const char *path)
{
    char ThreeChars[4];

    if(path == NULL)
        return FALSE;
    if (path[0] == 0)
        return FALSE;

    strncpy(ThreeChars, path, 3);
    ThreeChars[3] = 0;

    if(ThreeChars[0] == DIR_CHAR)
        return TRUE;

#ifdef  CMS_IS_WINDOWS_
    if (isalpha((int) ThreeChars[0]) && ThreeChars[1] == ':')
        return TRUE;
#endif
    return FALSE;
}


// Makes a file path based on a given reference path
// NOTE: this function doesn't check if the path exists or even if it's legal
static
cmsBool BuildAbsolutePath(const char *relPath, const char *basePath, char *buffer, cmsUInt32Number MaxLen)
{
    char *tail;
    cmsUInt32Number len;

    // Already absolute?
    if (isabsolutepath(relPath)) {

        strncpy(buffer, relPath, MaxLen);
        buffer[MaxLen-1] = 0;
        return TRUE;
    }

    // No, search for last
    strncpy(buffer, basePath, MaxLen);
    buffer[MaxLen-1] = 0;

    tail = strrchr(buffer, DIR_CHAR);
    if (tail == NULL) return FALSE;    // Is not absolute and has no separators??

    len = (cmsUInt32Number) (tail - buffer);
    if (len >= MaxLen) return FALSE;

    // No need to assure zero terminator over here
    strncpy(tail + 1, relPath, MaxLen - len);

    return TRUE;
}


// Make sure no exploit is being even tried
static
const char* NoMeta(const char* str)
{
    if (strchr(str, '%') != NULL)
        return "**** CORRUPTED FORMAT STRING ***";

    return str;
}

// Syntax error
static
cmsBool SynError(cmsContext ContextID, cmsIT8* it8, const char *Txt, ...)
{
    char Buffer[256], ErrMsg[1024];
    va_list args;

    va_start(args, Txt);
    vsnprintf(Buffer, 255, Txt, args);
    Buffer[255] = 0;
    va_end(args);

    snprintf(ErrMsg, 1023, "%s: Line %d, %s", it8->FileStack[it8 ->IncludeSP]->FileName, it8->lineno, Buffer);
    ErrMsg[1023] = 0;
    it8->sy = SSYNERROR;
    cmsSignalError(ContextID, cmsERROR_CORRUPTION_DETECTED, "%s", ErrMsg);
    return FALSE;
}

// Check if current symbol is same as specified. issue an error else.
static
cmsBool Check(cmsContext ContextID, cmsIT8* it8, SYMBOL sy, const char* Err)
{
        if (it8 -> sy != sy)
                return SynError(ContextID, it8, NoMeta(Err));
        return TRUE;
}

// Read Next character from stream
static
void NextCh(cmsIT8* it8)
{
    if (it8 -> FileStack[it8 ->IncludeSP]->Stream) {

        it8 ->ch = fgetc(it8 ->FileStack[it8 ->IncludeSP]->Stream);

        if (feof(it8 -> FileStack[it8 ->IncludeSP]->Stream))  {

            if (it8 ->IncludeSP > 0) {

                fclose(it8 ->FileStack[it8->IncludeSP--]->Stream);
                it8 -> ch = ' ';                            // Whitespace to be ignored

            } else
                it8 ->ch = 0;   // EOF
        }
    }
    else {
        it8->ch = *it8->Source;
        if (it8->ch) it8->Source++;
    }
}


// Try to see if current identifier is a keyword, if so return the referred symbol
static
SYMBOL BinSrchKey(const char *id)
{
    int l = 1;
    int r = NUMKEYS;
    int x, res;

    while (r >= l)
    {
        x = (l+r)/2;
        res = cmsstrcasecmp(id, TabKeys[x-1].id);
        if (res == 0) return TabKeys[x-1].sy;
        if (res < 0) r = x - 1;
        else l = x + 1;
    }

    return SUNDEFINED;
}


// 10 ^n
static
cmsFloat64Number xpow10(int n)
{
    return pow(10, (cmsFloat64Number) n);
}


//  Reads a Real number, tries to follow from integer number
static
void ReadReal(cmsIT8* it8, cmsInt32Number inum)
{
    it8->dnum = (cmsFloat64Number)inum;

    while (isdigit(it8->ch)) {

        it8->dnum = (cmsFloat64Number)it8->dnum * 10.0 + (cmsFloat64Number)(it8->ch - '0');
        NextCh(it8);
    }

    if (it8->ch == '.') {        // Decimal point

        cmsFloat64Number frac = 0.0;      // fraction
        int prec = 0;                     // precision

        NextCh(it8);               // Eats dec. point

        while (isdigit(it8->ch)) {

            frac = frac * 10.0 + (cmsFloat64Number)(it8->ch - '0');
            prec++;
            NextCh(it8);
        }

        it8->dnum = it8->dnum + (frac / xpow10(prec));
    }

    // Exponent, example 34.00E+20
    if (toupper(it8->ch) == 'E') {

        cmsInt32Number e;
        cmsInt32Number sgn;

        NextCh(it8); sgn = 1;

        if (it8->ch == '-') {

            sgn = -1; NextCh(it8);
        }
        else
            if (it8->ch == '+') {

                sgn = +1;
                NextCh(it8);
            }

        e = 0;
        while (isdigit(it8->ch)) {

            cmsInt32Number digit = (it8->ch - '0');

            if ((cmsFloat64Number)e * 10.0 + (cmsFloat64Number)digit < (cmsFloat64Number)+2147483647.0)
                e = e * 10 + digit;

            NextCh(it8);
        }

        e = sgn*e;
        it8->dnum = it8->dnum * xpow10(e);
    }
}

// Parses a float number
// This can not call directly atof because it uses locale dependent
// parsing, while CCMX files always use . as decimal separator
static
cmsFloat64Number ParseFloatNumber(const char *Buffer)
{
    cmsFloat64Number dnum = 0.0;
    int sign = 1;

    // keep safe
    if (Buffer == NULL) return 0.0;

    if (*Buffer == '-' || *Buffer == '+') {

        sign = (*Buffer == '-') ? -1 : 1;
        Buffer++;
    }


    while (*Buffer && isdigit((int)*Buffer)) {

        dnum = dnum * 10.0 + (*Buffer - '0');
        if (*Buffer) Buffer++;
    }

    if (*Buffer == '.') {

        cmsFloat64Number frac = 0.0;      // fraction
        int prec = 0;                     // precision

        if (*Buffer) Buffer++;

        while (*Buffer && isdigit((int)*Buffer)) {

            frac = frac * 10.0 + (*Buffer - '0');
            prec++;
            if (*Buffer) Buffer++;
        }

        dnum = dnum + (frac / xpow10(prec));
    }

    // Exponent, example 34.00E+20
    if (*Buffer && toupper(*Buffer) == 'E') {

        int e;
        int sgn;

        if (*Buffer) Buffer++;
        sgn = 1;

        if (*Buffer == '-') {

            sgn = -1;
            if (*Buffer) Buffer++;
        }
        else
            if (*Buffer == '+') {

                sgn = +1;
                if (*Buffer) Buffer++;
            }

        e = 0;
        while (*Buffer && isdigit((int)*Buffer)) {

            cmsInt32Number digit = (*Buffer - '0');

            if ((cmsFloat64Number)e * 10.0 + digit < (cmsFloat64Number)+2147483647.0)
                e = e * 10 + digit;

            if (*Buffer) Buffer++;
        }

        e = sgn*e;
        dnum = dnum * xpow10(e);
    }

    return sign * dnum;
}


// Reads a string, special case to avoid infinite resursion on .include
static
void InStringSymbol(cmsContext ContextID, cmsIT8* it8)
{
    while (isseparator(it8->ch))
        NextCh(it8);

    if (it8->ch == '\'' || it8->ch == '\"')
    {
        int sng;

        sng = it8->ch;
        StringClear(it8->str);

        NextCh(it8);

        while (it8->ch != sng) {

            if (it8->ch == '\n' || it8->ch == '\r' || it8->ch == 0) break;
            else {
                StringAppend(ContextID, it8->str, (char)it8->ch);
                NextCh(it8);
            }
        }

        it8->sy = SSTRING;
        NextCh(it8);
    }
    else
        SynError(ContextID, it8, "String expected");

}

// Reads next symbol
static
void InSymbol(cmsContext ContextID, cmsIT8* it8)
{
    SYMBOL key;

    do {

        while (isseparator(it8->ch))
            NextCh(it8);

        if (isfirstidchar(it8->ch)) {          // Identifier

            StringClear(it8->id);

            do {

                StringAppend(ContextID, it8->id, (char) it8->ch);

                NextCh(it8);

            } while (isidchar(it8->ch));


            key = BinSrchKey(StringPtr(it8->id));
            if (key == SUNDEFINED) it8->sy = SIDENT;
            else it8->sy = key;

        }
        else                         // Is a number?
            if (isdigit(it8->ch) || it8->ch == '.' || it8->ch == '-' || it8->ch == '+')
            {
                int sign = 1;

                if (it8->ch == '-') {
                    sign = -1;
                    NextCh(it8);
                }

                it8->inum = 0;
                it8->sy   = SINUM;

                if (it8->ch == '0') {          // 0xnnnn (Hexa) or 0bnnnn (Binary)

                    NextCh(it8);
                    if (toupper(it8->ch) == 'X') {

                        int j;

                        NextCh(it8);
                        while (isxdigit(it8->ch))
                        {
                            it8->ch = toupper(it8->ch);
                            if (it8->ch >= 'A' && it8->ch <= 'F')  j = it8->ch -'A'+10;
                            else j = it8->ch - '0';

                            if ((cmsFloat64Number) it8->inum * 16.0 + (cmsFloat64Number) j > (cmsFloat64Number)+2147483647.0)
                            {
                                SynError(ContextID, it8, "Invalid hexadecimal number");
                                it8->sy = SEOF;
                                return;
                            }

                            it8->inum = it8->inum * 16 + j;
                            NextCh(it8);
                        }
                        return;
                    }

                    if (toupper(it8->ch) == 'B') {  // Binary

                        int j;

                        NextCh(it8);
                        while (it8->ch == '0' || it8->ch == '1')
                        {
                            j = it8->ch - '0';

                            if ((cmsFloat64Number) it8->inum * 2.0 + j > (cmsFloat64Number)+2147483647.0)
                            {
                                SynError(ContextID, it8, "Invalid binary number");
                                it8->sy = SEOF;
                                return;
                            }

                            it8->inum = it8->inum * 2 + j;
                            NextCh(it8);
                        }
                        return;
                    }
                }


                while (isdigit(it8->ch)) {

                    cmsInt32Number digit = (it8->ch - '0');

                    if ((cmsFloat64Number) it8->inum * 10.0 + (cmsFloat64Number) digit > (cmsFloat64Number) +2147483647.0) {
                        ReadReal(it8, it8->inum);
                        it8->sy = SDNUM;
                        it8->dnum *= sign;
                        return;
                    }

                    it8->inum = it8->inum * 10 + digit;
                    NextCh(it8);
                }

                if (it8->ch == '.') {

                    ReadReal(it8, it8->inum);
                    it8->sy = SDNUM;
                    it8->dnum *= sign;
                    return;
                }

                it8 -> inum *= sign;

                // Special case. Numbers followed by letters are taken as identifiers

                if (isidchar(it8 ->ch)) {

                    char buffer[127];

                    if (it8 ->sy == SINUM) {

                        snprintf(buffer, sizeof(buffer), "%d", it8->inum);
                    }
                    else {

                        snprintf(buffer, sizeof(buffer), it8 ->DoubleFormatter, it8->dnum);
                    }

                    StringCat(ContextID, it8->id, buffer);

                    do {

                        StringAppend(ContextID, it8->id, (char) it8->ch);

                        NextCh(it8);

                    } while (isidchar(it8->ch));

                    it8->sy = SIDENT;
                }
                return;

            }
            else
                switch ((int) it8->ch) {

        // Eof stream markers
        case '\x1a':
        case 0:
        case -1:
            it8->sy = SEOF;
            break;


        // Next line
        case '\r':
            NextCh(it8);
            if (it8 ->ch == '\n')
                NextCh(it8);
            it8->sy = SEOLN;
            it8->lineno++;
            break;

        case '\n':
            NextCh(it8);
            it8->sy = SEOLN;
            it8->lineno++;
            break;

        // Comment
        case '#':
            NextCh(it8);
            while (it8->ch && it8->ch != '\n' && it8->ch != '\r')
                NextCh(it8);

            it8->sy = SCOMMENT;
            break;

        // String.
        case '\'':
        case '\"':
            InStringSymbol(ContextID, it8);
            break;


        default:
            SynError(ContextID, it8, "Unrecognized character: 0x%x", it8 ->ch);
            it8->sy = SEOF;
            return;
            }

    } while (it8->sy == SCOMMENT);

    // Handle the include special token

    if (it8 -> sy == SINCLUDE) {

                FILECTX* FileNest;

                if(it8 -> IncludeSP >= (MAXINCLUDE-1)) {

                    SynError(ContextID, it8, "Too many recursion levels");
                    it8->sy = SEOF;
                    return;
                }

                InStringSymbol(ContextID, it8);
                if (!Check(ContextID, it8, SSTRING, "Filename expected"))
                {
                    it8->sy = SEOF;
                    return;
                }

                FileNest = it8 -> FileStack[it8 -> IncludeSP + 1];
                if(FileNest == NULL) {

                    FileNest = it8 ->FileStack[it8 -> IncludeSP + 1] = (FILECTX*)AllocChunk(ContextID, it8, sizeof(FILECTX));
                    if (FileNest == NULL) {
                        SynError(ContextID, it8, "Out of memory");
                        it8->sy = SEOF;
                        return;
                    }
                }

                if (BuildAbsolutePath(StringPtr(it8->str),
                                      it8->FileStack[it8->IncludeSP]->FileName,
                                      FileNest->FileName, cmsMAX_PATH-1) == FALSE) {
                    SynError(ContextID, it8, "File path too long");
                    it8->sy = SEOF;
                    return;
                }

                FileNest->Stream = fopen(FileNest->FileName, "rt");
                if (FileNest->Stream == NULL) {

                        SynError(ContextID, it8, "File %s not found", FileNest->FileName);
                        it8->sy = SEOF;
                        return;
                }
                it8->IncludeSP++;

                it8 ->ch = ' ';
                InSymbol(ContextID, it8);
    }

}

// Checks end of line separator
static
cmsBool CheckEOLN(cmsContext ContextID, cmsIT8* it8)
{
        if (!Check(ContextID, it8, SEOLN, "Expected separator")) return FALSE;
        while (it8 -> sy == SEOLN)
                        InSymbol(ContextID, it8);
        return TRUE;

}

// Skip a symbol

static
void Skip(cmsContext ContextID, cmsIT8* it8, SYMBOL sy)
{
        if (it8->sy == sy && it8->sy != SEOF)
                        InSymbol(ContextID, it8);
}


// Skip multiple EOLN
static
void SkipEOLN(cmsContext ContextID, cmsIT8* it8)
{
    while (it8->sy == SEOLN) {
             InSymbol(ContextID, it8);
    }
}


// Returns a string holding current value
static
cmsBool GetVal(cmsContext ContextID, cmsIT8* it8, char* Buffer, cmsUInt32Number max, const char* ErrorTitle)
{
    switch (it8->sy) {

    case SEOLN:   // Empty value
                  Buffer[0]=0;
                  break;
    case SIDENT:  strncpy(Buffer, StringPtr(it8->id), max);
                  Buffer[max-1]=0;
                  break;
    case SINUM:   snprintf(Buffer, max, "%d", it8 -> inum); break;
    case SDNUM:   snprintf(Buffer, max, it8->DoubleFormatter, it8 -> dnum); break;
    case SSTRING: strncpy(Buffer, StringPtr(it8->str), max);
                  Buffer[max-1] = 0;
                  break;


    default:
         return SynError(ContextID, it8, "%s", ErrorTitle);
    }

    Buffer[max] = 0;
    return TRUE;
}

// ---------------------------------------------------------- Table

static
TABLE* GetTable(cmsContext ContextID, cmsIT8* it8)
{
   if ((it8 -> nTable >= it8 ->TablesCount)) {

           SynError(ContextID, it8, "Table %d out of sequence", it8 -> nTable);
           return it8 -> Tab;
   }

   return it8 ->Tab + it8 ->nTable;
}

// ---------------------------------------------------------- Memory management


// Frees an allocator and owned memory
void CMSEXPORT cmsIT8Free(cmsContext ContextID, cmsHANDLE hIT8)
{
   cmsIT8* it8 = (cmsIT8*) hIT8;

    if (it8 == NULL)
        return;

    if (it8->MemorySink) {

        OWNEDMEM* p;
        OWNEDMEM* n;

        for (p = it8->MemorySink; p != NULL; p = n) {

            n = p->Next;
            if (p->Ptr) _cmsFree(ContextID, p->Ptr);
            _cmsFree(ContextID, p);
        }
    }

    if (it8->MemoryBlock)
        _cmsFree(ContextID, it8->MemoryBlock);

    _cmsFree(ContextID, it8);
}


// Allocates a chunk of data, keep linked list
static
void* AllocBigBlock(cmsContext ContextID, cmsIT8* it8, cmsUInt32Number size)
{
    OWNEDMEM* ptr1;
    void* ptr = _cmsMallocZero(ContextID, size);

    if (ptr != NULL) {

        ptr1 = (OWNEDMEM*) _cmsMallocZero(ContextID, sizeof(OWNEDMEM));

        if (ptr1 == NULL) {

            _cmsFree(ContextID, ptr);
            return NULL;
        }

        ptr1-> Ptr        = ptr;
        ptr1-> Next       = it8 -> MemorySink;
        it8 -> MemorySink = ptr1;
    }

    return ptr;
}


// Suballocator.
static
void* AllocChunk(cmsContext ContextID, cmsIT8* it8, cmsUInt32Number size)
{
    cmsUInt32Number Free = it8 ->Allocator.BlockSize - it8 ->Allocator.Used;
    cmsUInt8Number* ptr;

    size = _cmsALIGNMEM(size);

    if (size > Free) {

        if (it8 -> Allocator.BlockSize == 0)

                it8 -> Allocator.BlockSize = 20*1024;
        else
                it8 ->Allocator.BlockSize *= 2;

        if (it8 ->Allocator.BlockSize < size)
                it8 ->Allocator.BlockSize = size;

        it8 ->Allocator.Used = 0;
        it8 ->Allocator.Block = (cmsUInt8Number*)  AllocBigBlock(ContextID, it8, it8 ->Allocator.BlockSize);
    }

    if (it8->Allocator.Block == NULL)
        return NULL;

    ptr = it8 ->Allocator.Block + it8 ->Allocator.Used;
    it8 ->Allocator.Used += size;

    return (void*) ptr;

}


// Allocates a string
static
char *AllocString(cmsContext ContextID, cmsIT8* it8, const char* str)
{
    cmsUInt32Number Size = (cmsUInt32Number) strlen(str)+1;
    char *ptr;


    ptr = (char *) AllocChunk(ContextID, it8, Size);
    if (ptr) memcpy(ptr, str, Size-1);

    return ptr;
}

// Searches through linked list

static
cmsBool IsAvailableOnList(cmsContext ContextID, KEYVALUE* p, const char* Key, const char* Subkey, KEYVALUE** LastPtr)
{
    cmsUNUSED_PARAMETER(ContextID);
    if (LastPtr) *LastPtr = p;

    for (;  p != NULL; p = p->Next) {

        if (LastPtr) *LastPtr = p;

        if (*Key != '#') { // Comments are ignored

            if (cmsstrcasecmp(Key, p->Keyword) == 0)
                break;
        }
    }

    if (p == NULL)
        return FALSE;

    if (Subkey == 0)
        return TRUE;

    for (; p != NULL; p = p->NextSubkey) {

        if (p ->Subkey == NULL) continue;

        if (LastPtr) *LastPtr = p;

        if (cmsstrcasecmp(Subkey, p->Subkey) == 0)
            return TRUE;
    }

    return FALSE;
}



// Add a property into a linked list
static
KEYVALUE* AddToList(cmsContext ContextID, cmsIT8* it8, KEYVALUE** Head, const char *Key, const char *Subkey, const char* xValue, WRITEMODE WriteAs)
{
    KEYVALUE* p;
    KEYVALUE* last;


    // Check if property is already in list

    if (IsAvailableOnList(ContextID, *Head, Key, Subkey, &p)) {

        // This may work for editing properties

        //     return SynError(ContextID, it8, "duplicate key <%s>", Key);
    }
    else {

        last = p;

        // Allocate the container
        p = (KEYVALUE*) AllocChunk(ContextID, it8, sizeof(KEYVALUE));
        if (p == NULL)
        {
            SynError(ContextID, it8, "AddToList: out of memory");
            return NULL;
        }

        // Store name and value
        p->Keyword = AllocString(ContextID, it8, Key);
        p->Subkey = (Subkey == NULL) ? NULL : AllocString(ContextID, it8, Subkey);

        // Keep the container in our list
        if (*Head == NULL) {
            *Head = p;
        }
        else
        {
            if (Subkey != NULL && last != NULL) {

                last->NextSubkey = p;

                // If Subkey is not null, then last is the last property with the same key,
                // but not necessarily is the last property in the list, so we need to move
                // to the actual list end
                while (last->Next != NULL)
                         last = last->Next;
            }

            if (last != NULL) last->Next = p;
        }

        p->Next    = NULL;
        p->NextSubkey = NULL;
    }

    p->WriteAs = WriteAs;

    if (xValue != NULL) {

        p->Value   = AllocString(ContextID, it8, xValue);
    }
    else {
        p->Value   = NULL;
    }

    return p;
}

static
KEYVALUE* AddAvailableProperty(cmsContext ContextID, cmsIT8* it8, const char* Key, WRITEMODE as)
{
    return AddToList(ContextID, it8, &it8->ValidKeywords, Key, NULL, NULL, as);
}


static
KEYVALUE* AddAvailableSampleID(cmsContext ContextID, cmsIT8* it8, const char* Key)
{
    return AddToList(ContextID, it8, &it8->ValidSampleID, Key, NULL, NULL, WRITE_UNCOOKED);
}


static
void AllocTable(cmsContext ContextID, cmsIT8* it8)
{
    TABLE* t;
    cmsUNUSED_PARAMETER(ContextID);

    t = it8 ->Tab + it8 ->TablesCount;

    t->HeaderList = NULL;
    t->DataFormat = NULL;
    t->Data       = NULL;

    it8 ->TablesCount++;
}


cmsInt32Number CMSEXPORT cmsIT8SetTable(cmsContext ContextID, cmsHANDLE  IT8, cmsUInt32Number nTable)
{
     cmsIT8* it8 = (cmsIT8*) IT8;

     if (nTable >= it8 ->TablesCount) {

         if (nTable == it8 ->TablesCount) {

             AllocTable(ContextID, it8);
         }
         else {
             SynError(ContextID, it8, "Table %d is out of sequence", nTable);
             return -1;
         }
     }

     it8 ->nTable = nTable;

     return (cmsInt32Number) nTable;
}



// Init an empty container
cmsHANDLE  CMSEXPORT cmsIT8Alloc(cmsContext ContextID)
{
    cmsIT8* it8;
    cmsUInt32Number i;

    it8 = (cmsIT8*) _cmsMallocZero(ContextID, sizeof(cmsIT8));
    if (it8 == NULL) return NULL;

    AllocTable(ContextID, it8);

    it8->MemoryBlock = NULL;
    it8->MemorySink  = NULL;

    it8 ->nTable = 0;

    it8->Allocator.Used = 0;
    it8->Allocator.Block = NULL;
    it8->Allocator.BlockSize = 0;

    it8->ValidKeywords = NULL;
    it8->ValidSampleID = NULL;

    it8 -> sy = SUNDEFINED;
    it8 -> ch = ' ';
    it8 -> Source = NULL;
    it8 -> inum = 0;
    it8 -> dnum = 0.0;

    it8->FileStack[0] = (FILECTX*)AllocChunk(ContextID, it8, sizeof(FILECTX));
    it8->IncludeSP   = 0;
    it8 -> lineno = 1;

    it8->id = StringAlloc(ContextID, it8, MAXSTR);
    it8->str = StringAlloc(ContextID, it8, MAXSTR);

    strcpy(it8->DoubleFormatter, DEFAULT_DBL_FORMAT);
    cmsIT8SetSheetType(ContextID, (cmsHANDLE) it8, "CGATS.17");

    // Initialize predefined properties & data

    for (i=0; i < NUMPREDEFINEDPROPS; i++)
            AddAvailableProperty(ContextID, it8, PredefinedProperties[i].id, PredefinedProperties[i].as);

    for (i=0; i < NUMPREDEFINEDSAMPLEID; i++)
            AddAvailableSampleID(ContextID, it8, PredefinedSampleID[i]);


   return (cmsHANDLE) it8;
}


const char* CMSEXPORT cmsIT8GetSheetType(cmsContext ContextID, cmsHANDLE hIT8)
{
        return GetTable(ContextID, (cmsIT8*) hIT8)->SheetType;
}

cmsBool CMSEXPORT cmsIT8SetSheetType(cmsContext ContextID, cmsHANDLE hIT8, const char* Type)
{
        TABLE* t = GetTable(ContextID, (cmsIT8*) hIT8);

        strncpy(t ->SheetType, Type, MAXSTR-1);
        t ->SheetType[MAXSTR-1] = 0;
        return TRUE;
}

cmsBool CMSEXPORT cmsIT8SetComment(cmsContext ContextID, cmsHANDLE hIT8, const char* Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    if (!Val) return FALSE;
    if (!*Val) return FALSE;

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, "# ", NULL, Val, WRITE_UNCOOKED) != NULL;
}

// Sets a property
cmsBool CMSEXPORT cmsIT8SetPropertyStr(cmsContext ContextID, cmsHANDLE hIT8, const char* Key, const char *Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    if (!Val) return FALSE;
    if (!*Val) return FALSE;

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, Key, NULL, Val, WRITE_STRINGIFY) != NULL;
}

cmsBool CMSEXPORT cmsIT8SetPropertyDbl(cmsContext ContextID, cmsHANDLE hIT8, const char* cProp, cmsFloat64Number Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    char Buffer[1024];

    snprintf(Buffer, 1023, it8->DoubleFormatter, Val);

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, cProp, NULL, Buffer, WRITE_UNCOOKED) != NULL;
}

cmsBool CMSEXPORT cmsIT8SetPropertyHex(cmsContext ContextID, cmsHANDLE hIT8, const char* cProp, cmsUInt32Number Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    char Buffer[1024];

    snprintf(Buffer, 1023, "%u", Val);

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, cProp, NULL, Buffer, WRITE_HEXADECIMAL) != NULL;
}

cmsBool CMSEXPORT cmsIT8SetPropertyUncooked(cmsContext ContextID, cmsHANDLE hIT8, const char* Key, const char* Buffer)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, Key, NULL, Buffer, WRITE_UNCOOKED) != NULL;
}

cmsBool CMSEXPORT cmsIT8SetPropertyMulti(cmsContext ContextID, cmsHANDLE hIT8, const char* Key, const char* SubKey, const char *Buffer)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    return AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, Key, SubKey, Buffer, WRITE_PAIR) != NULL;
}

// Gets a property
const char* CMSEXPORT cmsIT8GetProperty(cmsContext ContextID, cmsHANDLE hIT8, const char* Key)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    KEYVALUE* p;

    if (IsAvailableOnList(ContextID, GetTable(ContextID, it8) -> HeaderList, Key, NULL, &p))
    {
        return p -> Value;
    }
    return NULL;
}


cmsFloat64Number CMSEXPORT cmsIT8GetPropertyDbl(cmsContext ContextID, cmsHANDLE hIT8, const char* cProp)
{
    const char *v = cmsIT8GetProperty(ContextID, hIT8, cProp);

    if (v == NULL) return 0.0;

    return ParseFloatNumber(v);
}

const char* CMSEXPORT cmsIT8GetPropertyMulti(cmsContext ContextID, cmsHANDLE hIT8, const char* Key, const char *SubKey)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    KEYVALUE* p;

    if (IsAvailableOnList(ContextID, GetTable(ContextID, it8) -> HeaderList, Key, SubKey, &p)) {
        return p -> Value;
    }
    return NULL;
}

// ----------------------------------------------------------------- Datasets

// A safe atoi that returns 0 when NULL input is given
static
cmsInt32Number satoi(const char* b)
{
    int n;

    if (b == NULL) return 0;

    n = atoi(b);
    if (n > 0x7fffffffL) return 0x7fffffffL;
    if (n < -0x7ffffffeL) return -0x7ffffffeL;

    return (cmsInt32Number)n;
}


static
cmsBool AllocateDataFormat(cmsContext ContextID, cmsIT8* it8)
{
    TABLE* t = GetTable(ContextID, it8);

    if (t -> DataFormat) return TRUE;    // Already allocated

    t -> nSamples  = satoi(cmsIT8GetProperty(ContextID, it8, "NUMBER_OF_FIELDS"));

    if (t -> nSamples <= 0) {

        SynError(ContextID, it8, "AllocateDataFormat: Unknown NUMBER_OF_FIELDS");
        return FALSE;
    }

    t -> DataFormat = (char**) AllocChunk (ContextID, it8, ((cmsUInt32Number) t->nSamples + 1) * sizeof(char *));
    if (t->DataFormat == NULL) {

        SynError(ContextID, it8, "AllocateDataFormat: Unable to allocate dataFormat array");
        return FALSE;
    }

    return TRUE;
}

static
const char *GetDataFormat(cmsContext ContextID, cmsIT8* it8, int n)
{
    TABLE* t = GetTable(ContextID, it8);

    if (t->DataFormat)
        return t->DataFormat[n];

    return NULL;
}

static
cmsBool SetDataFormat(cmsContext ContextID, cmsIT8* it8, int n, const char *label)
{
    TABLE* t = GetTable(ContextID, it8);

    if (!t->DataFormat) {

        if (!AllocateDataFormat(ContextID, it8))
            return FALSE;
    }

    if (n > t -> nSamples) {
        SynError(ContextID, it8, "More than NUMBER_OF_FIELDS fields.");
        return FALSE;
    }

    if (t->DataFormat) {
        t->DataFormat[n] = AllocString(ContextID, it8, label);
        if (t->DataFormat[n] == NULL) return FALSE;
    }

    return TRUE;
}


cmsBool CMSEXPORT cmsIT8SetDataFormat(cmsContext ContextID, cmsHANDLE  h, int n, const char *Sample)
{
    cmsIT8* it8 = (cmsIT8*)h;
    return SetDataFormat(ContextID, it8, n, Sample);
}

// Convert to binary
static
const char* satob(const char* v)
{
    cmsUInt32Number x;
    static char buf[33];
    char *s = buf + 33;

    if (v == NULL) return "0";

    x = atoi(v);
    *--s = 0;
    if (!x) *--s = '0';
    for (; x; x /= 2) *--s = '0' + x%2;

    return s;
}


static
cmsBool AllocateDataSet(cmsContext ContextID, cmsIT8* it8)
{
    TABLE* t = GetTable(ContextID, it8);

    if (t -> Data) return TRUE;    // Already allocated

    t-> nSamples   = satoi(cmsIT8GetProperty(ContextID, it8, "NUMBER_OF_FIELDS"));
    t-> nPatches   = satoi(cmsIT8GetProperty(ContextID, it8, "NUMBER_OF_SETS"));

    if (t -> nSamples < 0 || t->nSamples > 0x7ffe || t->nPatches < 0 || t->nPatches > 0x7ffe)
    {
        SynError(ContextID, it8, "AllocateDataSet: too much data");
        return FALSE;
    }
    else {
        // Some dumb analizers warns of possible overflow here, just take a look couple of lines above.
        t->Data = (char**)AllocChunk(ContextID, it8, ((cmsUInt32Number)t->nSamples + 1) * ((cmsUInt32Number)t->nPatches + 1) * sizeof(char*));
        if (t->Data == NULL) {

            SynError(ContextID, it8, "AllocateDataSet: Unable to allocate data array");
            return FALSE;
        }
    }

    return TRUE;
}

static
char* GetData(cmsContext ContextID, cmsIT8* it8, int nSet, int nField)
{
    TABLE* t = GetTable(ContextID, it8);
    int nSamples    = t -> nSamples;
    int nPatches    = t -> nPatches;

    if (nSet >= nPatches || nField >= nSamples)
        return NULL;

    if (!t->Data) return NULL;
    return t->Data [nSet * nSamples + nField];
}

static
cmsBool SetData(cmsContext ContextID, cmsIT8* it8, int nSet, int nField, const char *Val)
{
    TABLE* t = GetTable(ContextID, it8);

    if (!t->Data) {
        if (!AllocateDataSet(ContextID, it8)) return FALSE;
    }

    if (!t->Data) return FALSE;

    if (nSet > t -> nPatches || nSet < 0) {

            return SynError(ContextID, it8, "Patch %d out of range, there are %d patches", nSet, t -> nPatches);
    }

    if (nField > t ->nSamples || nField < 0) {
            return SynError(ContextID, it8, "Sample %d out of range, there are %d samples", nField, t ->nSamples);

    }

    t->Data [nSet * t -> nSamples + nField] = AllocString(ContextID, it8, Val);
    return TRUE;
}


// --------------------------------------------------------------- File I/O


// Writes a string to file
static
void WriteStr(cmsContext ContextID, SAVESTREAM* f, const char *str)
{
    cmsUInt32Number len;
    cmsUNUSED_PARAMETER(ContextID);

    if (str == NULL)
        str = " ";

    // Length to write
    len = (cmsUInt32Number) strlen(str);
    f ->Used += len;


    if (f ->stream) {   // Should I write it to a file?

        if (fwrite(str, 1, len, f->stream) != len) {
            cmsSignalError(0, cmsERROR_WRITE, "Write to file error in CGATS parser");
            return;
        }

    }
    else {  // Or to a memory block?

        if (f ->Base) {   // Am I just counting the bytes?

            if (f ->Used > f ->Max) {

                 cmsSignalError(0, cmsERROR_WRITE, "Write to memory overflows in CGATS parser");
                 return;
            }

            memmove(f ->Ptr, str, len);
            f->Ptr += len;
        }

    }
}


// Write formatted

static
void Writef(cmsContext ContextID, SAVESTREAM* f, const char* frm, ...)
{
    char Buffer[4096];
    va_list args;

    va_start(args, frm);
    vsnprintf(Buffer, 4095, frm, args);
    Buffer[4095] = 0;
    WriteStr(ContextID, f, Buffer);
    va_end(args);

}

// Writes full header
static
void WriteHeader(cmsContext ContextID, cmsIT8* it8, SAVESTREAM* fp)
{
    KEYVALUE* p;
    TABLE* t = GetTable(ContextID, it8);

    // Writes the type
    WriteStr(ContextID, fp, t->SheetType);
    WriteStr(ContextID, fp, "\n");

    for (p = t->HeaderList; (p != NULL); p = p->Next)
    {
        if (*p ->Keyword == '#') {

            char* Pt;

            WriteStr(ContextID, fp, "#\n# ");
            for (Pt = p ->Value; *Pt; Pt++) {


                Writef(ContextID, fp, "%c", *Pt);

                if (*Pt == '\n') {
                    WriteStr(ContextID, fp, "# ");
                }
            }

            WriteStr(ContextID, fp, "\n#\n");
            continue;
        }


        if (!IsAvailableOnList(ContextID, it8-> ValidKeywords, p->Keyword, NULL, NULL)) {

#ifdef CMS_STRICT_CGATS
            WriteStr(ContextID, fp, "KEYWORD\t\"");
            WriteStr(ContextID, fp, p->Keyword);
            WriteStr(ContextID, fp, "\"\n");
#endif

            AddAvailableProperty(ContextID, it8, p->Keyword, WRITE_UNCOOKED);
        }

        WriteStr(ContextID, fp, p->Keyword);
        if (p->Value) {

            switch (p ->WriteAs) {

            case WRITE_UNCOOKED:
                    Writef(ContextID, fp, "\t%s", p ->Value);
                    break;

            case WRITE_STRINGIFY:
                    Writef(ContextID, fp, "\t\"%s\"", p->Value );
                    break;

            case WRITE_HEXADECIMAL:
                    Writef(ContextID, fp, "\t0x%X", satoi(p ->Value));
                    break;

            case WRITE_BINARY:
                    Writef(ContextID, fp, "\t0b%s", satob(p ->Value));
                    break;

            case WRITE_PAIR:
                    Writef(ContextID, fp, "\t\"%s,%s\"", p->Subkey, p->Value);
                    break;

            default: SynError(ContextID, it8, "Unknown write mode %d", p ->WriteAs);
                     return;
            }
        }

        WriteStr(ContextID, fp, "\n");
    }

}


// Writes the data format
static
void WriteDataFormat(cmsContext ContextID, SAVESTREAM* fp, cmsIT8* it8)
{
    int i, nSamples;
    TABLE* t = GetTable(ContextID, it8);

    if (!t -> DataFormat) return;

       WriteStr(ContextID, fp, "BEGIN_DATA_FORMAT\n");
       WriteStr(ContextID, fp, " ");
       nSamples = satoi(cmsIT8GetProperty(ContextID, it8, "NUMBER_OF_FIELDS"));

       for (i = 0; i < nSamples; i++) {

              WriteStr(ContextID, fp, t->DataFormat[i]);
              WriteStr(ContextID, fp, ((i == (nSamples-1)) ? "\n" : "\t"));
          }

       WriteStr (ContextID, fp, "END_DATA_FORMAT\n");
}


// Writes data array
static
void WriteData(cmsContext ContextID, SAVESTREAM* fp, cmsIT8* it8)
{
       int  i, j;
       TABLE* t = GetTable(ContextID, it8);

       if (!t->Data) return;

       WriteStr (ContextID, fp, "BEGIN_DATA\n");

       t->nPatches = satoi(cmsIT8GetProperty(ContextID, it8, "NUMBER_OF_SETS"));

       for (i = 0; i < t-> nPatches; i++) {

              WriteStr(ContextID, fp, " ");

              for (j = 0; j < t->nSamples; j++) {

                     char *ptr = t->Data[i*t->nSamples+j];

                     if (ptr == NULL) WriteStr(ContextID, fp, "\"\"");
                     else {
                         // If value contains whitespace, enclose within quote

                         if (strchr(ptr, ' ') != NULL) {

                             WriteStr(ContextID, fp, "\"");
                             WriteStr(ContextID, fp, ptr);
                             WriteStr(ContextID, fp, "\"");
                         }
                         else
                            WriteStr(ContextID, fp, ptr);
                     }

                     WriteStr(ContextID, fp, ((j == (t->nSamples-1)) ? "\n" : "\t"));
              }
       }
       WriteStr(ContextID, fp, "END_DATA\n");
}



// Saves whole file
cmsBool CMSEXPORT cmsIT8SaveToFile(cmsContext ContextID, cmsHANDLE hIT8, const char* cFileName)
{
    SAVESTREAM sd;
    cmsUInt32Number i;
    cmsIT8* it8 = (cmsIT8*) hIT8;

    memset(&sd, 0, sizeof(sd));

    sd.stream = fopen(cFileName, "wt");
    if (!sd.stream) return FALSE;

    for (i=0; i < it8 ->TablesCount; i++) {

            cmsIT8SetTable(ContextID, hIT8, i);
            WriteHeader(ContextID, it8, &sd);
            WriteDataFormat(ContextID, &sd, it8);
            WriteData(ContextID, &sd, it8);
    }

    if (fclose(sd.stream) != 0) return FALSE;

    return TRUE;
}


// Saves to memory
cmsBool CMSEXPORT cmsIT8SaveToMem(cmsContext ContextID, cmsHANDLE hIT8, void *MemPtr, cmsUInt32Number* BytesNeeded)
{
    SAVESTREAM sd;
    cmsUInt32Number i;
    cmsIT8* it8 = (cmsIT8*) hIT8;

    memset(&sd, 0, sizeof(sd));

    sd.stream = NULL;
    sd.Base   = (cmsUInt8Number*)  MemPtr;
    sd.Ptr    = sd.Base;

    sd.Used = 0;

    if (sd.Base)
        sd.Max  = *BytesNeeded;     // Write to memory?
    else
        sd.Max  = 0;                // Just counting the needed bytes

    for (i=0; i < it8 ->TablesCount; i++) {

        cmsIT8SetTable(ContextID, hIT8, i);
        WriteHeader(ContextID, it8, &sd);
        WriteDataFormat(ContextID, &sd, it8);
        WriteData(ContextID, &sd, it8);
    }

    sd.Used++;  // The \0 at the very end

    if (sd.Base)
        *sd.Ptr = 0;

    *BytesNeeded = sd.Used;

    return TRUE;
}


// -------------------------------------------------------------- Higher level parsing

static
cmsBool DataFormatSection(cmsContext ContextID, cmsIT8* it8)
{
    int iField = 0;
    TABLE* t = GetTable(ContextID, it8);

    InSymbol(ContextID, it8);   // Eats "BEGIN_DATA_FORMAT"
    CheckEOLN(ContextID, it8);

    while (it8->sy != SEND_DATA_FORMAT &&
        it8->sy != SEOLN &&
        it8->sy != SEOF &&
        it8->sy != SSYNERROR)  {

            if (it8->sy != SIDENT) {

                return SynError(ContextID, it8, "Sample type expected");
            }

            if (!SetDataFormat(ContextID, it8, iField, StringPtr(it8->id))) return FALSE;
            iField++;

            InSymbol(ContextID, it8);
            SkipEOLN(ContextID, it8);
       }

       SkipEOLN(ContextID, it8);
       Skip(ContextID, it8, SEND_DATA_FORMAT);
       SkipEOLN(ContextID, it8);

       if (iField != t ->nSamples) {
           SynError(ContextID, it8, "Count mismatch. NUMBER_OF_FIELDS was %d, found %d\n", t ->nSamples, iField);


       }

       return TRUE;
}



static
cmsBool DataSection (cmsContext ContextID, cmsIT8* it8)
{
    int  iField = 0;
    int  iSet   = 0;
    char Buffer[256];
    TABLE* t = GetTable(ContextID, it8);

    InSymbol(ContextID, it8);   // Eats "BEGIN_DATA"
    CheckEOLN(ContextID, it8);

    if (!t->Data) {
        if (!AllocateDataSet(ContextID, it8)) return FALSE;
    }

    while (it8->sy != SEND_DATA && it8->sy != SEOF)
    {
        if (iField >= t -> nSamples) {
            iField = 0;
            iSet++;

        }

        if (it8->sy != SEND_DATA && it8->sy != SEOF) {

            switch (it8->sy)
            {

            // To keep very long data
            case SIDENT:
                if (!SetData(ContextID, it8, iSet, iField, StringPtr(it8->id)))
                    return FALSE;
                break;

            case SSTRING:
                if (!SetData(ContextID, it8, iSet, iField, StringPtr(it8->str)))
                    return FALSE;
                break;

            default:

            if (!GetVal(ContextID, it8, Buffer, 255, "Sample data expected"))
                return FALSE;

            if (!SetData(ContextID, it8, iSet, iField, Buffer))
                return FALSE;
            }

            iField++;

            InSymbol(ContextID, it8);
            SkipEOLN(ContextID, it8);
        }
    }

    SkipEOLN(ContextID, it8);
    Skip(ContextID, it8, SEND_DATA);
    SkipEOLN(ContextID, it8);

    // Check for data completion.

    if ((iSet+1) != t -> nPatches)
        return SynError(ContextID, it8, "Count mismatch. NUMBER_OF_SETS was %d, found %d\n", t ->nPatches, iSet+1);

    return TRUE;
}




static
cmsBool HeaderSection(cmsContext ContextID, cmsIT8* it8)
{
    char VarName[MAXID];
    char Buffer[MAXSTR];
    KEYVALUE* Key;

        while (it8->sy != SEOF &&
               it8->sy != SSYNERROR &&
               it8->sy != SBEGIN_DATA_FORMAT &&
               it8->sy != SBEGIN_DATA) {


        switch (it8 -> sy) {

        case SKEYWORD:
                InSymbol(ContextID, it8);
                if (!GetVal(ContextID, it8, Buffer, MAXSTR-1, "Keyword expected")) return FALSE;
                if (!AddAvailableProperty(ContextID, it8, Buffer, WRITE_UNCOOKED)) return FALSE;
                InSymbol(ContextID, it8);
                break;


        case SDATA_FORMAT_ID:
                InSymbol(ContextID, it8);
                if (!GetVal(ContextID, it8, Buffer, MAXSTR-1, "Keyword expected")) return FALSE;
                if (!AddAvailableSampleID(ContextID, it8, Buffer)) return FALSE;
                InSymbol(ContextID, it8);
                break;


        case SIDENT:
            strncpy(VarName, StringPtr(it8->id), MAXID - 1);
            VarName[MAXID - 1] = 0;

            if (!IsAvailableOnList(ContextID, it8->ValidKeywords, VarName, NULL, &Key)) {

#ifdef CMS_STRICT_CGATS
                return SynError(ContextID, it8, "Undefined keyword '%s'", VarName);
#else
                Key = AddAvailableProperty(ContextID, it8, VarName, WRITE_UNCOOKED);
                if (Key == NULL) return FALSE;
#endif
            }

            InSymbol(ContextID, it8);
            if (!GetVal(ContextID, it8, Buffer, MAXSTR - 1, "Property data expected")) return FALSE;

            if (Key->WriteAs != WRITE_PAIR) {
                AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, VarName, NULL, Buffer,
                    (it8->sy == SSTRING) ? WRITE_STRINGIFY : WRITE_UNCOOKED);
            }
            else {
                const char *Subkey;
                char *Nextkey;
                if (it8->sy != SSTRING)
                    return SynError(ContextID, it8, "Invalid value '%s' for property '%s'.", Buffer, VarName);

                // chop the string as a list of "subkey, value" pairs, using ';' as a separator
                for (Subkey = Buffer; Subkey != NULL; Subkey = Nextkey)
                {
                    char *Value, *temp;

                    //  identify token pair boundary
                    Nextkey = (char*)strchr(Subkey, ';');
                    if (Nextkey)
                        *Nextkey++ = '\0';

                    // for each pair, split the subkey and the value
                    Value = (char*)strrchr(Subkey, ',');
                    if (Value == NULL)
                        return SynError(ContextID, it8, "Invalid value for property '%s'.", VarName);

                    // gobble the spaces before the coma, and the coma itself
                    temp = Value++;
                    do *temp-- = '\0'; while (temp >= Subkey && *temp == ' ');

                    // gobble any space at the right
                    temp = Value + strlen(Value) - 1;
                    while (*temp == ' ') *temp-- = '\0';

                    // trim the strings from the left
                    Subkey += strspn(Subkey, " ");
                    Value += strspn(Value, " ");

                    if (Subkey[0] == 0 || Value[0] == 0)
                        return SynError(ContextID, it8, "Invalid value for property '%s'.", VarName);
                    AddToList(ContextID, it8, &GetTable(ContextID, it8)->HeaderList, VarName, Subkey, Value, WRITE_PAIR);
                }
            }

            InSymbol(ContextID, it8);
            break;


        case SEOLN: break;

        default:
                return SynError(ContextID, it8, "expected keyword or identifier");
        }

    SkipEOLN(ContextID, it8);
    }

    return TRUE;

}


static
void ReadType(cmsIT8* it8, char* SheetTypePtr)
{
    cmsInt32Number cnt = 0;

    // First line is a very special case.

    while (isseparator(it8->ch))
            NextCh(it8);

    while (it8->ch != '\r' && it8 ->ch != '\n' && it8->ch != '\t' && it8 -> ch != 0) {

        if (cnt++ < MAXSTR)
            *SheetTypePtr++= (char) it8 ->ch;
        NextCh(it8);
    }

    *SheetTypePtr = 0;
}


static
cmsBool ParseIT8(cmsContext ContextID, cmsIT8* it8, cmsBool nosheet)
{
    char* SheetTypePtr = it8 ->Tab[0].SheetType;

    if (nosheet == 0) {
        ReadType(it8, SheetTypePtr);
    }

    InSymbol(ContextID, it8);

    SkipEOLN(ContextID, it8);

    while (it8-> sy != SEOF &&
           it8-> sy != SSYNERROR) {

            switch (it8 -> sy) {

            case SBEGIN_DATA_FORMAT:
                    if (!DataFormatSection(ContextID, it8)) return FALSE;
                    break;

            case SBEGIN_DATA:

                    if (!DataSection(ContextID, it8)) return FALSE;

                    if (it8 -> sy != SEOF) {

                            AllocTable(ContextID, it8);
                            it8 ->nTable = it8 ->TablesCount - 1;

                            // Read sheet type if present. We only support identifier and string.
                            // <ident> <eoln> is a type string
                            // anything else, is not a type string
                            if (nosheet == 0) {

                                if (it8 ->sy == SIDENT) {

                                    // May be a type sheet or may be a prop value statement. We cannot use insymbol in
                                    // this special case...
                                     while (isseparator(it8->ch))
                                         NextCh(it8);

                                     // If a newline is found, then this is a type string
                                    if (it8 ->ch == '\n' || it8->ch == '\r') {

                                         cmsIT8SetSheetType(ContextID, it8, StringPtr(it8 ->id));
                                         InSymbol(ContextID, it8);
                                    }
                                    else
                                    {
                                        // It is not. Just continue
                                        cmsIT8SetSheetType(ContextID, it8, "");
                                    }
                                }
                                else
                                    // Validate quoted strings
                                    if (it8 ->sy == SSTRING) {
                                        cmsIT8SetSheetType(ContextID, it8, StringPtr(it8 ->str));
                                        InSymbol(ContextID, it8);
                                    }
                           }

                    }
                    break;

            case SEOLN:
                    SkipEOLN(ContextID, it8);
                    break;

            default:
                    if (!HeaderSection(ContextID, it8)) return FALSE;
           }

    }

    return (it8 -> sy != SSYNERROR);
}



// Init useful pointers

static
void CookPointers(cmsContext ContextID, cmsIT8* it8)
{
    int idField, i;
    char* Fld;
    cmsUInt32Number j;
    cmsUInt32Number nOldTable = it8 ->nTable;

    for (j=0; j < it8 ->TablesCount; j++) {

    TABLE* t = it8 ->Tab + j;

    t -> SampleID = 0;
    it8 ->nTable = j;

    for (idField = 0; idField < t -> nSamples; idField++)
    {
        if (t ->DataFormat == NULL){
            SynError(ContextID, it8, "Undefined DATA_FORMAT");
            return;
        }

        Fld = t->DataFormat[idField];
        if (!Fld) continue;


        if (cmsstrcasecmp(Fld, "SAMPLE_ID") == 0) {

            t -> SampleID = idField;
        }

        // "LABEL" is an extension. It keeps references to forward tables

        if ((cmsstrcasecmp(Fld, "LABEL") == 0) || Fld[0] == '$') {

            // Search for table references...
            for (i = 0; i < t->nPatches; i++) {

                char *Label = GetData(ContextID, it8, i, idField);

                if (Label) {

                    cmsUInt32Number k;

                    // This is the label, search for a table containing
                    // this property

                    for (k = 0; k < it8->TablesCount; k++) {

                        TABLE* Table = it8->Tab + k;
                        KEYVALUE* p;

                        if (IsAvailableOnList(ContextID, Table->HeaderList, Label, NULL, &p)) {

                            // Available, keep type and table
                            char Buffer[256];

                            char* Type = p->Value;
                            int  nTable = (int)k;

                            snprintf(Buffer, 255, "%s %d %s", Label, nTable, Type);

                            SetData(ContextID, it8, i, idField, Buffer);
                        }
                    }


                }

            }


        }

	}
	}

    it8 ->nTable = nOldTable;
}

// Try to infere if the file is a CGATS/IT8 file at all. Read first line
// that should be something like some printable characters plus a \n
// returns 0 if this is not like a CGATS, or an integer otherwise. This integer is the number of words in first line?
static
int IsMyBlock(const cmsUInt8Number* Buffer, cmsUInt32Number n)
{
    int words = 1, space = 0, quot = 0;
    cmsUInt32Number i;

    if (n < 10) return 0;   // Too small

    if (n > 132)
        n = 132;

    for (i = 1; i < n; i++) {

        switch(Buffer[i])
        {
        case '\n':
        case '\r':
            return ((quot == 1) || (words > 2)) ? 0 : words;
        case '\t':
        case ' ':
            if(!quot && !space)
                space = 1;
            break;
        case '\"':
            quot = !quot;
            break;
        default:
            if (Buffer[i] < 32) return 0;
            if (Buffer[i] > 127) return 0;
            words += space;
            space = 0;
            break;
        }
    }

    return 0;
}


static
cmsBool IsMyFile(const char* FileName)
{
   FILE *fp;
   cmsUInt32Number Size;
   cmsUInt8Number Ptr[133];

   fp = fopen(FileName, "rt");
   if (!fp) {
       cmsSignalError(0, cmsERROR_FILE, "File '%s' not found", FileName);
       return FALSE;
   }

   Size = (cmsUInt32Number) fread(Ptr, 1, 132, fp);

   if (fclose(fp) != 0)
       return FALSE;

   Ptr[Size] = '\0';

   return IsMyBlock(Ptr, Size);
}

// ---------------------------------------------------------- Exported routines


cmsHANDLE  CMSEXPORT cmsIT8LoadFromMem(cmsContext ContextID, const void *Ptr, cmsUInt32Number len)
{
    cmsHANDLE hIT8;
    cmsIT8*  it8;
    int type;

    _cmsAssert(Ptr != NULL);
    _cmsAssert(len != 0);

    type = IsMyBlock((const cmsUInt8Number*)Ptr, len);
    if (type == 0) return NULL;

    hIT8 = cmsIT8Alloc(ContextID);
    if (!hIT8) return NULL;

    it8 = (cmsIT8*) hIT8;
    it8 ->MemoryBlock = (char*) _cmsMalloc(ContextID, len + 1);
    if (it8->MemoryBlock == NULL)
    {
        cmsIT8Free(ContextID, hIT8);
        return FALSE;
    }

    strncpy(it8 ->MemoryBlock, (const char*) Ptr, len);
    it8 ->MemoryBlock[len] = 0;

    strncpy(it8->FileStack[0]->FileName, "", cmsMAX_PATH-1);
    it8-> Source = it8 -> MemoryBlock;

    if (!ParseIT8(ContextID, it8, type-1)) {

        cmsIT8Free(ContextID, hIT8);
        return FALSE;
    }

    CookPointers(ContextID, it8);
    it8 ->nTable = 0;

    _cmsFree(ContextID, it8->MemoryBlock);
    it8 -> MemoryBlock = NULL;

    return hIT8;


}


cmsHANDLE  CMSEXPORT cmsIT8LoadFromFile(cmsContext ContextID, const char* cFileName)
{

     cmsHANDLE hIT8;
     cmsIT8*  it8;
     int type;

     _cmsAssert(cFileName != NULL);

     type = IsMyFile(cFileName);
     if (type == 0) return NULL;

     hIT8 = cmsIT8Alloc(ContextID);
     it8 = (cmsIT8*) hIT8;
     if (!hIT8) return NULL;


     it8 ->FileStack[0]->Stream = fopen(cFileName, "rt");

     if (!it8 ->FileStack[0]->Stream) {
         cmsIT8Free(ContextID, hIT8);
         return NULL;
     }


    strncpy(it8->FileStack[0]->FileName, cFileName, cmsMAX_PATH-1);
    it8->FileStack[0]->FileName[cmsMAX_PATH-1] = 0;

    if (!ParseIT8(ContextID, it8, type-1)) {

            fclose(it8 ->FileStack[0]->Stream);
            cmsIT8Free(ContextID, hIT8);
            return NULL;
    }

    CookPointers(ContextID, it8);
    it8 ->nTable = 0;

    if (fclose(it8 ->FileStack[0]->Stream)!= 0) {
            cmsIT8Free(ContextID, hIT8);
            return NULL;
    }

    return hIT8;

}

int CMSEXPORT cmsIT8EnumDataFormat(cmsContext ContextID, cmsHANDLE hIT8, char ***SampleNames)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    TABLE* t;

    _cmsAssert(hIT8 != NULL);

    t = GetTable(ContextID, it8);

    if (SampleNames)
        *SampleNames = t -> DataFormat;
    return t -> nSamples;
}


cmsUInt32Number CMSEXPORT cmsIT8EnumProperties(cmsContext ContextID, cmsHANDLE hIT8, char ***PropertyNames)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    KEYVALUE* p;
    cmsUInt32Number n;
    char **Props;
    TABLE* t;

    _cmsAssert(hIT8 != NULL);

    t = GetTable(ContextID, it8);

    // Pass#1 - count properties

    n = 0;
    for (p = t -> HeaderList;  p != NULL; p = p->Next) {
        n++;
    }


    Props = (char **) AllocChunk(ContextID, it8, sizeof(char *) * n);
    if (Props != NULL) {

	// Pass#2 - Fill pointers
	n = 0;
	for (p = t->HeaderList; p != NULL; p = p->Next) {
		Props[n++] = p->Keyword;
	}

    }
    *PropertyNames = Props;

    return n;
}

cmsUInt32Number CMSEXPORT cmsIT8EnumPropertyMulti(cmsContext ContextID, cmsHANDLE hIT8, const char* cProp, const char ***SubpropertyNames)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    KEYVALUE *p, *tmp;
    cmsUInt32Number n;
    const char **Props;
    TABLE* t;

    _cmsAssert(hIT8 != NULL);


    t = GetTable(ContextID, it8);

    if(!IsAvailableOnList(ContextID, t->HeaderList, cProp, NULL, &p)) {
        *SubpropertyNames = 0;
        return 0;
    }

    // Pass#1 - count properties

    n = 0;
    for (tmp = p;  tmp != NULL; tmp = tmp->NextSubkey) {
        if(tmp->Subkey != NULL)
            n++;
    }


    Props = (const char **) AllocChunk(ContextID, it8, sizeof(char *) * n);
    if (Props != NULL) {

        // Pass#2 - Fill pointers
        n = 0;
        for (tmp = p; tmp != NULL; tmp = tmp->NextSubkey) {
            if (tmp->Subkey != NULL)
                Props[n++] = p->Subkey;
        }
    }

    *SubpropertyNames = Props;
    return n;
}

static
int LocatePatch(cmsContext ContextID, cmsIT8* it8, const char* cPatch)
{
    int i;
    const char *data;
    TABLE* t = GetTable(ContextID, it8);

    for (i=0; i < t-> nPatches; i++) {

        data = GetData(ContextID, it8, i, t->SampleID);

        if (data != NULL) {

                if (cmsstrcasecmp(data, cPatch) == 0)
                        return i;
                }
        }

        // SynError(ContextID, it8, "Couldn't find patch '%s'\n", cPatch);
        return -1;
}


static
int LocateEmptyPatch(cmsContext ContextID, cmsIT8* it8)
{
    int i;
    const char *data;
    TABLE* t = GetTable(ContextID, it8);

    for (i=0; i < t-> nPatches; i++) {

        data = GetData(ContextID, it8, i, t->SampleID);

        if (data == NULL)
            return i;

    }

    return -1;
}

static
int LocateSample(cmsContext ContextID, cmsIT8* it8, const char* cSample)
{
    int i;
    const char *fld;
    TABLE* t = GetTable(ContextID, it8);

    for (i=0; i < t->nSamples; i++) {

        fld = GetDataFormat(ContextID, it8, i);
        if (fld != NULL) {
            if (cmsstrcasecmp(fld, cSample) == 0)
                return i;
        }
    }

    return -1;

}


int CMSEXPORT cmsIT8FindDataFormat(cmsContext ContextID, cmsHANDLE hIT8, const char* cSample)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    _cmsAssert(hIT8 != NULL);

    return LocateSample(ContextID, it8, cSample);
}



const char* CMSEXPORT cmsIT8GetDataRowCol(cmsContext ContextID, cmsHANDLE hIT8, int row, int col)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    _cmsAssert(hIT8 != NULL);

    return GetData(ContextID, it8, row, col);
}


cmsFloat64Number CMSEXPORT cmsIT8GetDataRowColDbl(cmsContext ContextID, cmsHANDLE hIT8, int row, int col)
{
    const char* Buffer;

    Buffer = cmsIT8GetDataRowCol(ContextID, hIT8, row, col);

    if (Buffer == NULL) return 0.0;

    return ParseFloatNumber(Buffer);
}


cmsBool CMSEXPORT cmsIT8SetDataRowCol(cmsContext ContextID, cmsHANDLE hIT8, int row, int col, const char* Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;

    _cmsAssert(hIT8 != NULL);

    return SetData(ContextID, it8, row, col, Val);
}


cmsBool CMSEXPORT cmsIT8SetDataRowColDbl(cmsContext ContextID, cmsHANDLE hIT8, int row, int col, cmsFloat64Number Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    char Buff[256];

    _cmsAssert(hIT8 != NULL);

    snprintf(Buff, 255, it8->DoubleFormatter, Val);

    return SetData(ContextID, it8, row, col, Buff);
}



const char* CMSEXPORT cmsIT8GetData(cmsContext ContextID, cmsHANDLE hIT8, const char* cPatch, const char* cSample)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    int iField, iSet;

    _cmsAssert(hIT8 != NULL);

    iField = LocateSample(ContextID, it8, cSample);
    if (iField < 0) {
        return NULL;
    }

    iSet = LocatePatch(ContextID, it8, cPatch);
    if (iSet < 0) {
            return NULL;
    }

    return GetData(ContextID, it8, iSet, iField);
}


cmsFloat64Number CMSEXPORT cmsIT8GetDataDbl(cmsContext ContextID, cmsHANDLE  it8, const char* cPatch, const char* cSample)
{
    const char* Buffer;

    Buffer = cmsIT8GetData(ContextID, it8, cPatch, cSample);

    return ParseFloatNumber(Buffer);
}



cmsBool CMSEXPORT cmsIT8SetData(cmsContext ContextID, cmsHANDLE hIT8, const char* cPatch, const char* cSample, const char *Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    int iField, iSet;
    TABLE* t;

    _cmsAssert(hIT8 != NULL);

    t = GetTable(ContextID, it8);

    iField = LocateSample(ContextID, it8, cSample);

    if (iField < 0)
        return FALSE;

    if (t-> nPatches == 0) {

        if (!AllocateDataFormat(ContextID, it8))
            return FALSE;

        if (!AllocateDataSet(ContextID, it8))
            return FALSE;

        CookPointers(ContextID, it8);
    }

    if (cmsstrcasecmp(cSample, "SAMPLE_ID") == 0) {

        iSet   = LocateEmptyPatch(ContextID, it8);
        if (iSet < 0) {
            return SynError(ContextID, it8, "Couldn't add more patches '%s'\n", cPatch);
        }

        iField = t -> SampleID;
    }
    else {
        iSet = LocatePatch(ContextID, it8, cPatch);
        if (iSet < 0) {
            return FALSE;
        }
    }

    return SetData(ContextID, it8, iSet, iField, Val);
}


cmsBool CMSEXPORT cmsIT8SetDataDbl(cmsContext ContextID, cmsHANDLE hIT8, const char* cPatch,
                                   const char* cSample,
                                   cmsFloat64Number Val)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    char Buff[256];

    _cmsAssert(hIT8 != NULL);

    snprintf(Buff, 255, it8->DoubleFormatter, Val);
    return cmsIT8SetData(ContextID, hIT8, cPatch, cSample, Buff);
}

// Buffer should get MAXSTR at least

const char* CMSEXPORT cmsIT8GetPatchName(cmsContext ContextID, cmsHANDLE hIT8, int nPatch, char* buffer)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    TABLE* t;
    char* Data;

    _cmsAssert(hIT8 != NULL);

    t = GetTable(ContextID, it8);
    Data = GetData(ContextID, it8, nPatch, t->SampleID);

    if (!Data) return NULL;
    if (!buffer) return Data;

    strncpy(buffer, Data, MAXSTR-1);
    buffer[MAXSTR-1] = 0;
    return buffer;
}

int CMSEXPORT cmsIT8GetPatchByName(cmsContext ContextID, cmsHANDLE hIT8, const char *cPatch)
{
    _cmsAssert(hIT8 != NULL);
    cmsUNUSED_PARAMETER(ContextID);

    return LocatePatch(ContextID, (cmsIT8*)hIT8, cPatch);
}

cmsUInt32Number CMSEXPORT cmsIT8TableCount(cmsContext ContextID, cmsHANDLE hIT8)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    cmsUNUSED_PARAMETER(ContextID);

    _cmsAssert(hIT8 != NULL);

    return it8 ->TablesCount;
}

// This handles the "LABEL" extension.
// Label, nTable, Type

int CMSEXPORT cmsIT8SetTableByLabel(cmsContext ContextID, cmsHANDLE hIT8, const char* cSet, const char* cField, const char* ExpectedType)
{
    const char* cLabelFld;
    char Type[256], Label[256];
    cmsUInt32Number nTable;

    _cmsAssert(hIT8 != NULL);

    if (cField != NULL && *cField == 0)
            cField = "LABEL";

    if (cField == NULL)
            cField = "LABEL";

    cLabelFld = cmsIT8GetData(ContextID, hIT8, cSet, cField);
    if (!cLabelFld) return -1;

    if (sscanf(cLabelFld, "%255s %u %255s", Label, &nTable, Type) != 3)
            return -1;

    if (ExpectedType != NULL && *ExpectedType == 0)
        ExpectedType = NULL;

    if (ExpectedType) {

        if (cmsstrcasecmp(Type, ExpectedType) != 0) return -1;
    }

    return cmsIT8SetTable(ContextID, hIT8, nTable);
}


cmsBool CMSEXPORT cmsIT8SetIndexColumn(cmsContext ContextID, cmsHANDLE hIT8, const char* cSample)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    int pos;

    _cmsAssert(hIT8 != NULL);

    pos = LocateSample(ContextID, it8, cSample);
    if(pos == -1)
        return FALSE;

    it8->Tab[it8->nTable].SampleID = pos;
    return TRUE;
}


void CMSEXPORT cmsIT8DefineDblFormat(cmsContext ContextID, cmsHANDLE hIT8, const char* Formatter)
{
    cmsIT8* it8 = (cmsIT8*) hIT8;
    cmsUNUSED_PARAMETER(ContextID);

    _cmsAssert(hIT8 != NULL);

    if (Formatter == NULL)
        strcpy(it8->DoubleFormatter, DEFAULT_DBL_FORMAT);
    else
        strncpy(it8->DoubleFormatter, Formatter, sizeof(it8->DoubleFormatter));

    it8 ->DoubleFormatter[sizeof(it8 ->DoubleFormatter)-1] = 0;
}
