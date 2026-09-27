/* ziputil.h — 最小 ZIP 提取（deflate/store），供 GUI 更新功能使用 */
#ifndef ZIPUTIL_H
#define ZIPUTIL_H

/* 从 zip 中提取「文件名以 nameSuffix 结尾」的条目，解压写入 dest（覆盖）。
   支持 store(0) 与 deflate(8)；不支持 zip64。
   返回 0 成功，-1 失败。 */
int ZipExtractEntry(const wchar_t *zipPath, const wchar_t *nameSuffix,
                    const wchar_t *dest);

#endif
