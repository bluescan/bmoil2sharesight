#pragma once
#define set(verStr) namespace Bmoil2ShareSightVersion { extern int Major, Minor, Revision; struct Parser { Parser(const char*);  }; static Parser parser(#verStr); }

set("BMOIL2SHARESIGHT_VERSION" "0.1.0")

#undef set
