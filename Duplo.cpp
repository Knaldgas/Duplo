#include "Duplo.h"
#include "ArgumentParser.h"
#include "HashUtil.h"
#include "SourceFile.h"
#include "SourceLine.h"
#include "StringUtil.h"
#include "TextFile.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <tuple>

enum class MatchType : unsigned char {
    NONE,
    MATCH
};
typedef std::tuple<unsigned, std::string> FileLength;

class ProcessResult {
    unsigned m_blocks;
    unsigned m_duplicateLines;
public:
    ProcessResult()
        : m_blocks(0),
          m_duplicateLines(0) {
    }

    ProcessResult(unsigned blocks, unsigned duplicateLines)
        : m_blocks(blocks),
          m_duplicateLines(duplicateLines) {
    }

    unsigned Blocks() const {
        return m_blocks;
    }

    unsigned DuplicateLines() const {
        return m_duplicateLines;
    }

    friend ProcessResult operator<<(ProcessResult& left, const ProcessResult& right);
};

ProcessResult operator<<(ProcessResult& left, const ProcessResult& right) {
    left.m_blocks += right.m_blocks;
    left.m_duplicateLines += right.m_duplicateLines;
    return left;
}

namespace {
    bool isSameFilename(const std::string& filename1, const std::string& filename2) {
        return StringUtil::GetFilenamePart(filename1) == StringUtil::GetFilenamePart(filename2);
    }

    std::tuple<std::vector<SourceFile>, std::vector<MatchType>, unsigned, unsigned> LoadSourceFiles(
        const std::vector<std::string>& lines,
        unsigned minChars,
        bool ignorePrepStuff) {

        std::vector<SourceFile> sourceFiles;
        std::vector<MatchType> matrix;
        unsigned maxLinesPerFile = 0;
        int files = 0;
        unsigned long locsTotal = 0;
        std::vector<FileLength> longestFiles;
        auto addSorted = [&longestFiles](int numLines, const std::string& filename) {
            longestFiles.emplace_back(numLines, filename);
            std::sort(
                std::begin(longestFiles),
                std::end(longestFiles),
                [](auto l, auto r) { return std::get<0>(l) > std::get<0>(r); });
            if (longestFiles.size() > 10)
                longestFiles.resize(10);
        };

        // Create vector with all source files
        for (unsigned i = 0; i < lines.size(); i++) {
            if (lines[i].size() > 5) {
                SourceFile pSourceFile(lines[i], minChars, ignorePrepStuff);
                unsigned numLines = pSourceFile.GetNumOfLines();
                if (numLines > 0) {
                    files++;
                    sourceFiles.push_back(std::move(pSourceFile));
                    locsTotal += numLines;
                    if (maxLinesPerFile < numLines) {
                        maxLinesPerFile = numLines;
                    }

                    // keep 10 worst case files
                    if (longestFiles.size() < 10) {
                        addSorted(numLines, lines[i]);
                    } else {
                        auto [l, _] = longestFiles.back();
                        if (l < numLines) {
                            addSorted(numLines, lines[i]);
                        }
                    }
                }
            }
        }

        if (maxLinesPerFile * maxLinesPerFile > matrix.max_size()) {
            std::ostringstream stream;
            stream
                << "Some files have too many lines. You can have files with approximately "
                << std::sqrt(matrix.max_size())
                << " lines at most." << std::endl
                << "Longest files:" << std::endl;
            for (auto [l, f] : longestFiles) {
                stream << l << ": " << f << std::endl;
            }

            throw std::exception(stream.str().c_str());
        }

        std::cout
            << lines.size()
            << " done.\n\n";
        // Generate matrix large enough for all files
        try {
            matrix.resize(maxLinesPerFile * maxLinesPerFile);
        }
        catch (const std::bad_alloc& ex) {
            std::ostringstream stream;
            stream
                << ex.what() << std::endl
                << "Longest files:" << std::endl;
            for (auto [l, f] : longestFiles) {
                stream << l << ": " << f << std::endl;
            }

            throw std::exception(stream.str().c_str());
        }

        return std::tuple(std::move(sourceFiles), matrix, files, locsTotal);
    }

    unsigned ReportSeq(
        int line1,
        int line2,
        int count,
        bool xml,
        const SourceFile& source1,
        const SourceFile& source2,
        std::ostream& outFile) {
        unsigned duplicateLines = 0;
        if (xml) {
            outFile << "    <set LineCount=\"" << count << "\">" << std::endl;
            outFile << "        <block SourceFile=\"" << source1.GetFilename() << "\" StartLineNumber=\"" << source1.GetLine(line1).GetLineNumber() << "\"/>" << std::endl;
            outFile << "        <block SourceFile=\"" << source2.GetFilename() << "\" StartLineNumber=\"" << source2.GetLine(line2).GetLineNumber() << "\"/>" << std::endl;
            outFile << "        <lines xml:space=\"preserve\">" << std::endl;
            for (int j = 0; j < count; j++) {
                // replace various characters/ strings so that it doesn't upset the XML parser
                std::string tmpstr = source1.GetLine(j + line1).GetLine();

                // " --> '
                StringUtil::StrSub(tmpstr, "\'", "\"", -1);

                // & --> &amp;
                StringUtil::StrSub(tmpstr, "&amp;", "&", -1);

                // < --> &lt;
                StringUtil::StrSub(tmpstr, "&lt;", "<", -1);

                // > --> &gt;
                StringUtil::StrSub(tmpstr, "&gt;", ">", -1);

                outFile << "            <line Text=\"" << tmpstr << "\"/>" << std::endl;
                duplicateLines++;
            }

            outFile << "        </lines>" << std::endl;
            outFile << "    </set>" << std::endl;
        } else {
            outFile << source1.GetFilename() << "(" << source1.GetLine(line1).GetLineNumber() << ")" << std::endl;
            outFile << source2.GetFilename() << "(" << source2.GetLine(line2).GetLineNumber() << ")" << std::endl;
            for (int j = 0; j < count; j++) {
                outFile << source1.GetLine(j + line1).GetLine() << std::endl;
                duplicateLines++;
            }

            outFile << std::endl;
        }

        return duplicateLines;
    }

    ProcessResult Process(
        const SourceFile& source1,
        const SourceFile& source2,
        std::vector<MatchType>& matrix,
        unsigned minBlockSize,
        unsigned char blockPercentThreshold,
        bool xml,
        std::ostream& outFile) {
        unsigned m = source1.GetNumOfLines();
        unsigned n = source2.GetNumOfLines();

        // Reset matrix data
        std::fill(std::begin(matrix), std::begin(matrix) + m * n, MatchType::NONE);

        // Compute matrix
        for (unsigned y = 0; y < m; y++) {
            auto& line = source1.GetLine(y);
            for (unsigned x = 0; x < n; x++) {
                if (line == source2.GetLine(x)) {
                    matrix[x + n * y] = MatchType::MATCH;
                }
            }
        }

        // support reporting filtering by both:
        // - "lines of code duplicated", &
        // - "percentage of file duplicated"
        unsigned lMinBlockSize = std::max(
            minBlockSize,
            std::min(
                minBlockSize,
                (std::max(n, m) * 100) / blockPercentThreshold));

        unsigned blocks = 0;
        unsigned duplicateLines = 0;

        // Scan vertical part
        for (unsigned y = 0; y < m; y++) {
            unsigned seqLen = 0;
            int maxX = std::min(n, m - y);
            for (int x = 0; x < maxX; x++) {
                if (matrix[x + n * (y + x)] == MatchType::MATCH) {
                    seqLen++;
                } else {
                    if (seqLen >= lMinBlockSize) {
                        int line1 = y + x - seqLen;
                        int line2 = x - seqLen;
                        if (line1 != line2 || source1 != source2) {
                            duplicateLines += ReportSeq(line1, line2, seqLen, xml, source1, source2, outFile);
                            blocks++;
                        }
                    }

                    seqLen = 0;
                }
            }

            if (seqLen >= lMinBlockSize) {
                int line1 = m - seqLen;
                int line2 = n - seqLen;
                if (line1 != line2 || source1 != source2) {
                    duplicateLines += ReportSeq(line1, line2, seqLen, xml, source1, source2, outFile);
                    blocks++;
                }
            }
        }

        if (source1 != source2) {
            // Scan horizontal part
            for (unsigned x = 1; x < n; x++) {
                unsigned seqLen = 0;
                int maxY = std::min(m, n - x);
                for (int y = 0; y < maxY; y++) {
                    if (matrix[x + y + n * y] == MatchType::MATCH) {
                        seqLen++;
                    } else {
                        if (seqLen >= lMinBlockSize) {
                            duplicateLines += ReportSeq(y - seqLen, x + y - seqLen, seqLen, xml, source1, source2, outFile);
                            blocks++;
                        }
                        seqLen = 0;
                    }
                }

                if (seqLen >= lMinBlockSize) {
                    duplicateLines += ReportSeq(m - seqLen, n - seqLen, seqLen, xml, source1, source2, outFile);
                    blocks++;
                }
            }
        }

        return ProcessResult(blocks, duplicateLines);
    }
}

void Duplo::Run(
    unsigned minChars,
    bool ignorePrepStuff,
    unsigned minBlockSize,
    unsigned char blockPercentThreshold,
    bool xml,
    bool ignoreSameFilename,
    const std::string& listFilename,
    const std::string& outputFileName) {
    std::ofstream outfile(outputFileName.c_str(), std::ios::out | std::ios::binary);
    if (!outfile) {
        std::ostringstream stream;
        stream << "Error: Can't open file: " << outputFileName << std::endl;
        throw std::exception(stream.str().c_str());
    }

    double duration;
    clock_t start = clock();

    std::cout << "Loading and hashing files ... " << std::flush;

    TextFile listOfFiles(listFilename);
    auto lines = listOfFiles.ReadLines(true);

    auto [sourceFiles, matrix, files, locsTotal] =
        LoadSourceFiles(lines, minChars, ignorePrepStuff);

    ProcessResult processResultTotal;

    // Compare each file with each other
    for (unsigned i = 0; i < sourceFiles.size(); i++) {
        std::cout << sourceFiles[i].GetFilename();
        ProcessResult processResult =
            Process(sourceFiles[i], sourceFiles[i], matrix, minBlockSize, blockPercentThreshold, xml, outfile);
        for (unsigned j = i + 1; j < sourceFiles.size(); j++) {
            if (!ignoreSameFilename || !isSameFilename(sourceFiles[i].GetFilename(), sourceFiles[j].GetFilename())) {
                processResult << Process(sourceFiles[i], sourceFiles[j], matrix, minBlockSize, blockPercentThreshold, xml, outfile);
            }
        }

        if (processResult.Blocks() > 0) {
            std::cout << " found: " << processResult.Blocks() << " block(s)" << std::endl;
        } else {
            std::cout << " nothing found." << std::endl;
        }

        processResultTotal << processResult;
    }

    clock_t finish = clock();
    duration = (double)(static_cast<long long>(finish) - start) / CLOCKS_PER_SEC;
    std::cout << "Time: " << duration << " seconds" << std::endl;

    if (xml) {
        outfile
            << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            << std::endl
            << "<?xml-stylesheet href=\"duplo.xsl\" type=\"text/xsl\"?>"
            << std::endl
            << "<duplo version=\"" << VERSION << "\">"
            << std::endl
            << "    <check Min_block_size=\""
            << minBlockSize
            << "\" Min_char_line=\""
            << minChars
            << "\" Ignore_prepro=\""
            << (ignorePrepStuff ? "true" : "false")
            << "\" Ignore_same_filename=\""
            << (ignoreSameFilename ? "true" : "false")
            << "\">"
            << std::endl
            << "        <summary Num_files=\""
            << files
            << "\" Duplicate_blocks=\""
            << processResultTotal.Blocks()
            << "\" Total_lines_of_code=\""
            << locsTotal
            << "\" Duplicate_lines_of_code=\""
            << processResultTotal.DuplicateLines()
            << "\" Time=\""
            << duration
            << "\"/>"
            << std::endl
            << "    </check>"
            << std::endl
            << "</duplo>"
            << std::endl;
    } else {
        outfile << "Configuration: " << std::endl;
        outfile << "  Number of files: " << files << std::endl;
        outfile << "  Minimal block size: " << minBlockSize << std::endl;
        outfile << "  Minimal characters in line: " << minChars << std::endl;
        outfile << "  Ignore preprocessor directives: " << ignorePrepStuff << std::endl;
        outfile << "  Ignore same filenames: " << ignoreSameFilename << std::endl;
        outfile << std::endl;
        outfile << "Results: " << std::endl;
        outfile << "  Lines of code: " << locsTotal << std::endl;
        outfile << "  Duplicate lines of code: " << processResultTotal.DuplicateLines() << std::endl;
        outfile << "  Total " << processResultTotal.Blocks() << " duplicate block(s) found." << std::endl
                << std::endl;
        //outfile << "  Time: " << duration << " seconds" << std::endl;
    }
}
