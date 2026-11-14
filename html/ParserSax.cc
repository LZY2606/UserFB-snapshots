#include "ParserSax.h"

#include <cctype>
#include <cstring>
#include <stdexcept>

void htmlcxx::HTML::ParserSax::parse(const std::string &html)
{
//	std::cerr << "Parsing string" << std::endl;
	parse(html.c_str(), html.c_str() + html.length());
}

namespace
{
	// Streaming counterpart of ParserSax::skipHtmlComment(). Scans for the
	// end of a comment, starting just after "<!--". When the buffer does not
	// hold the full comment yet, returns 0 and sets *complete to false,
	// unless eof is set, in which case the comment runs to the end of the
	// buffer exactly like the one-shot parser treats it.
	const char *skipStreamComment(const char *c, const char *end, bool eof, bool *complete)
	{
		while (c != end)
		{
			if (*c++ == '-')
			{
				if (c == end)
				{
					if (eof) { *complete = true; return end; }
					*complete = false; return 0;
				}
				if (*c == '-')
				{
					const char *d = c;
					while (++c != end && isspace(*c)) ;
					if (c == end)
					{
						if (eof) { *complete = true; return end; }
						*complete = false; return 0;
					}
					if (*c == '>') { *complete = true; return c + 1; }
					c = d;
				}
			}
		}
		if (eof) { *complete = true; return end; }
		*complete = false; return 0;
	}

	// Streaming counterpart of ParserSax::skipHtmlTag(). Returns one past the
	// '>' closing the tag. An unterminated attribute quote is only settled
	// at eof: a later chunk may still bring the closing quote and change
	// where the tag really ends.
	const char *skipStreamTag(const char *c, const char *end, bool eof, bool *complete)
	{
		while (c != end && *c != '>')
		{
			if (*c != '=')
			{
				++c;
			}
			else
			{ // found an attribute
				++c;
				while (c != end && isspace(*c)) ++c;

				if (c == end) break;

				if (*c == '\"' || *c == '\'')
				{
					const char *save = c;
					char quote = *c++;
					const char *q = reinterpret_cast<const char*>(memchr(c, quote, end - c));
					if (q)
					{
						c = q + 1;
					}
					else
					{
						if (!eof) { *complete = false; return 0; }
						c = save;
						++c;
					}
				}
			}
		}

		if (c != end) { *complete = true; return c + 1; }
		if (eof) { *complete = true; return end; }
		*complete = false; return 0;
	}
}

void htmlcxx::HTML::ParserSax::beginStream()
{
	mCdata = false;
	mpLiteral = 0;
	mCurrentOffset = 0;
	mStreamBuf.clear();
	mStreamActive = true;
	this->beginParsing();
}

void htmlcxx::HTML::ParserSax::feed(const std::string &chunk)
{
	if (!mStreamActive)
		throw std::logic_error("ParserSax::feed called outside a stream session");
	if (chunk.empty()) return;
	mStreamBuf += chunk;
	this->consumeStream(false);
}

void htmlcxx::HTML::ParserSax::finishStream()
{
	if (!mStreamActive) return;
	this->consumeStream(true);
	mStreamBuf.clear();
	mStreamActive = false;
	this->endParsing();
}

void htmlcxx::HTML::ParserSax::consumeStream(bool eof)
{
	const char *begin = mStreamBuf.data();
	const char *end = begin + mStreamBuf.size();
	const char *c = begin;
	const char *tok = begin; // start of the not yet emitted input

	while (c != end)
	{
		// For some tags, the text inside them is considered literal and is
		// only closed by their </TAG> counterpart
		while (mpLiteral)
		{
			while (c != end && *c != '<') ++c;

			if (c == end)
			{
				if (eof && tok != c) { this->parseContent(tok, c); tok = c; }
				goto DONE;
			}

			const char *end_text = c;
			const char *p = c + 1;

			if (p == end)
			{
				// A '<' at the very end of the buffer: only the next chunk
				// (or the real EOF) tells what it starts
				if (!eof) goto DONE;
				c = p;
				continue;
			}

			if (*p == '/')
			{
				c = p + 1;
				const char *l = mpLiteral;
				while (*l && c != end && ::tolower(*c) == *l)
				{
					++c;
					++l;
				}

				// Incomplete closing candidate: wait for the next chunk
				if (*l && c == end && !eof) goto DONE;

				// FIXME: as in the one-shot parser, plaintext never closes
				if (!*l && strcmp(mpLiteral, "plaintext"))
				{
					while (c != end && isspace(*c)) ++c;
					if (c == end && !eof) goto DONE;

					if (c != end && *c == '>')
					{
						++c;
						if (tok != end_text) this->parseContent(tok, end_text);
						mpLiteral = 0;
						// Re-scan the closing tag in normal mode, like the
						// one-shot parser does
						c = end_text;
						tok = end_text;
						break;
					}
				}
				// Not a closing tag: the '<' is literal text, keep scanning
				continue;
			}
			else if (*p == '!')
			{
				// we may find a comment and we should support it
				if (end - p < 3 && !eof) goto DONE;

				if (end - p >= 3 && p[1] == '-' && p[2] == '-')
				{
					bool complete = false;
					const char *comment_end = skipStreamComment(p + 3, end, eof, &complete);
					if (!complete) goto DONE;
					c = comment_end;
				}
				else
				{
					c = p;
				}
				continue;
			}
			else
			{
				c = p;
				continue;
			}
		}

		if (c == end) break;

		if (*c == '<')
		{
			const char *d = c + 1;
			if (d == end)
			{
				if (!eof) goto DONE;
				++c; // a trailing '<' is text
				continue;
			}

			if (isalpha(*d))
			{
				// beginning of tag
				bool complete = false;
				const char *tag_end = skipStreamTag(d, end, eof, &complete);
				if (!complete) goto DONE;

				if (tok != c) this->parseContent(tok, c);
				this->parseHtmlTag(c, tag_end);
				c = tag_end;
				tok = tag_end;
				continue;
			}

			if (*d == '/')
			{
				const char *e = d + 1;
				if (e == end && !eof) goto DONE;

				bool complete = false;
				const char *tag_end = skipStreamTag(d, end, eof, &complete);
				if (!complete) goto DONE;

				if (tok != c) this->parseContent(tok, c);
				if (e != end && isalpha(*e))
				{
					// end of tag
					this->parseHtmlTag(c, tag_end);
				}
				else
				{
					// not a conforming end of tag, treat as comment
					// as Mozilla does
					this->parseComment(c, tag_end);
				}
				c = tag_end;
				tok = tag_end;
				continue;
			}

			if (*d == '!')
			{
				// comment
				const char *e = d + 1;
				if (end - e < 2 && !eof) goto DONE;

				bool complete = false;
				const char *comment_end;
				if (end - e >= 2 && e[0] == '-' && e[1] == '-')
					comment_end = skipStreamComment(e + 2, end, eof, &complete);
				else
					comment_end = skipStreamTag(d, end, eof, &complete);
				if (!complete) goto DONE;

				if (tok != c) this->parseContent(tok, c);
				this->parseComment(c, comment_end);
				c = comment_end;
				tok = comment_end;
				continue;
			}

			if (*d == '?' || *d == '%')
			{
				// something like <?xml or <%VBSCRIPT
				bool complete = false;
				const char *tag_end = skipStreamTag(d, end, eof, &complete);
				if (!complete) goto DONE;

				if (tok != c) this->parseContent(tok, c);
				this->parseComment(c, tag_end);
				c = tag_end;
				tok = tag_end;
				continue;
			}

			++c; // '<' followed by something else is text
			continue;
		}

		++c;
	}

	// There may be some text at the end of the document
	if (eof && tok != c) { this->parseContent(tok, c); tok = c; }

DONE:
	if (tok != begin) mStreamBuf.erase(0, tok - begin);
}
