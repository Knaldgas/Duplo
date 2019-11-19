#ifndef _SOURCELINE_H_
#define _SOURCELINE_H_

#include <string>
#include <vector>

class SourceLine {
    std::string m_line;
    int m_lineNumber;
    unsigned long hash;

public:
    /**
     * Creates a new text file. The file is accessed relative to current directory.
     */
    SourceLine(const std::string& line, int lineNumber);

    int getLineNumber() const;
    const std::string& getLine() const;
    friend bool operator==(const SourceLine&, const SourceLine&);
};

#endif
