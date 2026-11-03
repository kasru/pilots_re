/* Версия и описание, вшитые в бинарник (см. version.h).

   Нужны платформам без системы ресурсов: у Linux (ELF) эквивалента VERSIONINFO
   не существует вовсе, так что строка в образе плюс ключ -version — это и есть
   честная замена. У macOS дополнительно собирается Info.plist
   (assets/macos-Info.plist.in), но и там эта строка остаётся тем, что видно
   без otool.

   Символы внешние, не static: определение попадает в образ, а на них ссылается
   ключ -version, так что линкер не имеет повода их выбросить. */

#include "version.h"

const char PILOTS_VERSION_STRING[] = PILOTS_VERSION;
const char PILOTS_DESCRIPTION_STRING[] = PILOTS_DESCRIPTION;
