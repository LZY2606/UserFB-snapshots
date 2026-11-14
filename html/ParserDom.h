#ifndef __HTML_PARSER_DOM_H__
#define __HTML_PARSER_DOM_H__

#include "ParserSax.h"
#include "tree.h"

namespace htmlcxx
{
	namespace HTML
	{
		class ParserDom : public ParserSax
		{
			public:
				ParserDom() : mParsingDone(false) {}
				~ParserDom() {}

				const tree<Node> &parseTree(const std::string &html);
				const tree<Node> &getTree()
				{ return mHtmlTree; }

				/** Move the already completed root-level prefix of the live
				    tree into a new tree and return it. The returned tree has
				    the same virtual root and owns its nodes; the active
				    top-level branch and anything after it stays in getTree(). */
				tree<Node> takeCompleted();

			protected:
				virtual void beginParsing();

				virtual void foundTag(Node node, bool isEnd);
				virtual void foundText(Node node);
				virtual void foundComment(Node node);

				virtual void endParsing();

				void updateRootLength();

				tree<Node> mHtmlTree;
				tree<Node>::iterator mCurrentState;
				bool mParsingDone;
		};

		std::ostream &operator<<(std::ostream &stream, const tree<HTML::Node> &tr);
	} //namespace HTML
} //namespace htmlcxx

#endif
