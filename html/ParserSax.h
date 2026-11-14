#ifndef __HTML_PARSER_SAX_H__
#define __HTML_PARSER_SAX_H__

#include <string>

#include "Node.h"

namespace htmlcxx
{
	namespace HTML
	{
		class ParserSax
		{
			public:
				ParserSax() : mpLiteral(0), mCdata(false), mStreamActive(false) {}
				virtual ~ParserSax() {}

				/** Parse the html code */
				void parse(const std::string &html);

				template <typename _Iterator>
				void parse(_Iterator begin, _Iterator end);

				/** Start (or restart) an incremental parsing session.
				    Any previous lexer state is discarded and beginParsing()
				    is called once for the new session. */
				void beginStream();
				/** Advance the parse over one more chunk of input.
				    Throws std::logic_error if no stream session is active. */
				void feed(const std::string &chunk);
				/** Signal the real end of input, flushing any pending
				    tokens, and call endParsing() once. */
				void finishStream();

			protected:
				// Redefine this if you want to do some initialization before
				// the parsing
				virtual void beginParsing() {}

				virtual void foundTag(Node node, bool isEnd) {}
				virtual void foundText(Node node) {}
				virtual void foundComment(Node node) {}

				virtual void endParsing() {}


				template <typename _Iterator>
				void parse(_Iterator &begin, _Iterator &end,
						std::forward_iterator_tag);

				template <typename _Iterator>
				void parseHtmlTag(_Iterator b, _Iterator c);

				template <typename _Iterator>
				void parseContent(_Iterator b, _Iterator c);

				template <typename _Iterator>
				void parseComment(_Iterator b, _Iterator c);

				template <typename _Iterator>
				_Iterator skipHtmlTag(_Iterator ptr, _Iterator end);
				
				template <typename _Iterator>
				_Iterator skipHtmlComment(_Iterator ptr, _Iterator end);

				// Consume as much of mStreamBuf as possible, emitting the
				// tokens that are fully determined. With eof set, everything
				// left is flushed exactly like the one-shot parser does.
				void consumeStream(bool eof);

				unsigned long mCurrentOffset;
				const char *mpLiteral;
				bool mCdata;
				bool mStreamActive;
				std::string mStreamBuf;
		};

	}//namespace HTML
}//namespace htmlcxx

#include "ParserSax.tcc"

#endif
