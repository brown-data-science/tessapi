#ifndef TESSAPI_WRAPPER_H
#define TESSAPI_WRAPPER_H

// fixes an annoying macro issue on windows
#define NOMINMAX

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <Eigen/Core>

#include <string>
#include <list>
#include <vector>
#include <tuple>
#include <memory>
#include <functional>
#include <stdexcept>

enum FontAttribute
{
	PLAIN,
	BOLD,
	ITALIC,
	UNDER_LINED
};

struct FontAttrLabel
{
	FontAttrLabel()
	{ }

	FontAttrLabel(FontAttribute attr, uint16_t point_size, uint16_t start, uint16_t length) :
		attr(attr),
		point_size(point_size),
		start(start),
		length(length)
	{ }

	FontAttribute attr;
	uint16_t point_size;
	uint16_t start; // inclusive
	uint16_t length;
};

/*class FontAttrCollection
{
	std::vector<FontAttrLabel> attrCollection;
	
public:
	FontAttrCollection(int start_sz = 10) :
		attrCollection(start_sz)
	{ }

	inline void push_back(FontAttrLabel attrLabel)
	{
		attrCollection.push_back(attrLabel);
	}

	inline void push_back(FontAttrLabel&& attrLabel)
	{
		attrCollection.push_back(attrLabel);
	}

	inline size_t size()
	{
		return attrCollection.size();
	}

	inline void clear()
	{
		attrCollection.clear();
	}

	inline FontAttrLabel getLabelAtPoint(uint16_t index)
	{
		for (auto& i : attrCollection)
		{
			if (i.start <= index && i.start + i.length > index)
			{
				return i;
			}
		}
	}

	inline std::list<FontAttrLabel> getLabelsInSpan(uint16_t start, uint16_t length = 1)
	{
		std::list<FontAttrLabel> labels;

		for (auto& i : attrCollection)
		{
			if (i.start + i.length > start && i.start <= start + length)
			{
				labels.push_back(i);
			}
		}

		return labels;
	}
};*/


// wrapper class for the tesseract api that only exposes necessary functionality
// and in a memory managed way
class TessAPI_Wrapper : public tesseract::TessBaseAPI
{
private:
	std::function<std::string(std::string, float)> spellCheckCB = [](std::string token, float conf) { return token; };

	// holds image data until it is no longer needed
	Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> img_data;

public:
	TessAPI_Wrapper() :
		tesseract::TessBaseAPI()
	{
		if (this->Init(NULL, "eng"))
		{
			throw std::runtime_error("initialization of internal 'TessBaseAPI()' class failed");
		}
	}
	~TessAPI_Wrapper()
	{ }

	// pybind11 can't convey inheritance unless we essentially redefine the parent class, this is easier
	int MeanTextConf_wrap() { return this->MeanTextConf(); }
	void SetRectangle_wrap(int x, int y, int w, int h) { return this->SetRectangle(x, y, w, h); }
	bool SetVariable_wrap(std::string var, std::string value) { return this->SetVariable(var.c_str(), value.c_str()); }
	void ReadConfigFile_wrap(std::string file_name) { this->ReadConfigFile(file_name.c_str()); }

	void SetImage_cv2(Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> img)
	{
		// move data to class member to postpone deletion
		img_data = std::move(img);
		img_data.transposeInPlace();

		// flatten the image data
		Eigen::Map< Eigen::Matrix<uint8_t, Eigen::Dynamic, 1> > flat(img_data.data(), img_data.size());

		// use rows in place and of columns and columns in place of rows because we transposed the data
		this->SetImage(flat.data(), img_data.rows(), img_data.cols(), 1, img_data.rows());
	}

	void RegisterSpellCheckCallback(const std::function<std::string(std::string, float)> &callback)
	{
		spellCheckCB = callback;
	}

	void RemoveSpellCheckCallback()
	{
		spellCheckCB = [](std::string token, float conf) { return token; };
	}

	std::tuple<std::string, std::list<FontAttrLabel>> GetTextWithAttrs()
	{
		this->Recognize(0);

		std::unique_ptr<tesseract::ResultIterator> itr(this->GetIterator());
		std::list<FontAttrLabel> textAttrGroups;
		std::string fullText;

		// return if iterator is invalid
		if (itr.get() == nullptr)
		{
			return std::tuple<std::string, std::list<FontAttrLabel>>(fullText, textAttrGroups);
		}

		// we only care about isBold, isItalic, isUnderLined, and pointsize
		// these values are set by getWordAttributes() below
		bool isBold, isItalic, isUnderLined, isMonoSpace, isSerif, isSmallCaps;
		int pointsize, font_id;

		FontAttribute txtAttr;

		auto getWordAttributes = [&isBold, &isItalic, &isUnderLined, 
								  &isMonoSpace, &isSerif, &isSmallCaps,
								  &pointsize, &font_id, &txtAttr, &itr]
		{
			itr->WordFontAttributes(&isBold, &isItalic, &isUnderLined, &isMonoSpace, &isSerif, &isSmallCaps, &pointsize, &font_id);
			txtAttr = isBold ? BOLD : isItalic ? ITALIC : isUnderLined ? UNDER_LINED : PLAIN;
		};

		getWordAttributes();

		FontAttribute groupAttr = txtAttr;
		int groupPSize = pointsize;
		uint16_t groupStart = 0;
		uint16_t groupSize = 0;

		// iterate through OCRed words
		while (true)
		{
			std::unique_ptr<char[]> text(itr->GetUTF8Text(tesseract::RIL_WORD));

			if (text.get() != nullptr)
			{
				std::string spellcheckedText = spellCheckCB(text.get(), itr->Confidence(tesseract::RIL_WORD));
				fullText += spellcheckedText;
				groupSize += (uint16_t)spellcheckedText.size();
			}

			// if not the last word of the entire block of text
			if (!itr->IsAtFinalElement(tesseract::RIL_BLOCK, tesseract::RIL_WORD))
			{
				// add appropriate whitespace (" " or "\n") depending on whether this is the end of a line or not
				fullText += itr->IsAtFinalElement(tesseract::RIL_TEXTLINE, tesseract::RIL_WORD) ? "\n" : " ";
				groupSize++;
			}
			
			// our loop condition
			if (!itr->Next(tesseract::RIL_WORD)) break;

			getWordAttributes();

			// end this group and start a new one if needed
			if (txtAttr != groupAttr || abs(groupPSize - pointsize) >= groupPSize * 0.15)
			{
				textAttrGroups.push_back(FontAttrLabel(groupAttr, groupPSize, groupStart, groupSize));

				groupStart += groupSize;
				groupSize = 0;
				groupAttr = txtAttr;
				groupPSize = pointsize;
			}
		}

		// the last group will not be appended in the loop because the text will end before the group does
		textAttrGroups.push_back(FontAttrLabel(groupAttr, groupPSize, groupStart, groupSize));

		return std::tuple<std::string, std::list<FontAttrLabel>>(fullText, textAttrGroups);
	}

	/*std::string GetText()
	{
		this->Recognize(0);

		std::unique_ptr<tesseract::ResultIterator> itr(this->GetIterator());
		std::list<FontAttrLabel> textAttrGroups;
		std::string fullText;

		// return if iterator is invalid
		if (itr.get() == nullptr)
		{
			return std::tuple<std::string, std::list<FontAttrLabel>>(fullText, textAttrGroups);
		}
	}*/

	std::string GetText/*NoSpellCheck*/()
	{
		std::unique_ptr<char[]> text(this->GetUTF8Text());
		if (text.get() != nullptr)
		{
			return std::string(text.get());
		}
		else return "";
	}

	std::tuple<int, int> TotalConfidence()
	{
		std::unique_ptr<int[]> confidence_list(this->AllWordConfidences());
		int* confidence_iter = confidence_list.get();

		int total_confidence = 0;
		int num_words = 0;

		if (confidence_iter != nullptr)
		{
			while (*confidence_iter != -1)
			{
				total_confidence += *(confidence_iter++);
				num_words++;
			}
		}

		return std::tuple<int, int>(total_confidence, num_words);
	}
};

#endif
