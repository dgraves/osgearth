#include <osgEarth/XmlUtils>
#include <osg/Notify>
#include <algorithm>
#include <sstream>
#include <expat.h>

using namespace osgEarth;


static std::string EMPTY_VALUE = "";


std::string
trim( const std::string& in )
{
    // by Rodrigo C F Dias
    // http://www.codeproject.com/KB/stl/stdstringtrim.aspx
    std::string str = in;
    std::string::size_type pos = str.find_last_not_of(' ');
    if(pos != std::string::npos) {
        str.erase(pos + 1);
        pos = str.find_first_not_of(' ');
        if(pos != std::string::npos) str.erase(0, pos);
    }
    else str.erase(str.begin(), str.end());
    return str;
}


XmlNode::XmlNode()
{
    //NOP
}

XmlElement::XmlElement( const std::string& _name )
{
    name = _name;
}

XmlElement::XmlElement( const std::string& _name, const XmlAttributes& _attrs )
{
    name = _name;
    attrs = _attrs;
}

const std::string&
XmlElement::getName() const
{
    return name;
}

XmlAttributes&
XmlElement::getAttrs()
{
    return attrs;
}

const XmlAttributes&
XmlElement::getAttrs() const
{
    return attrs;
}

const std::string&
XmlElement::getAttr( const std::string& key ) const
{
    XmlAttributes::const_iterator i = attrs.find( key );
    return i != attrs.end()? i->second : EMPTY_VALUE;
}

XmlNodeList&
XmlElement::getChildren()
{
    return children;
}

const XmlNodeList&
XmlElement::getChildren() const
{
    return children;
}
        
XmlElement*
XmlElement::getSubElement( const std::string& name ) const
{
    std::string name_lower = name;
    std::transform( name_lower.begin(), name_lower.end(), name_lower.begin(), tolower );

    for( XmlNodeList::const_iterator i = getChildren().begin(); i != getChildren().end(); i++ )
    {
        if ( i->get()->isElement() )
        {
            XmlElement* e = (XmlElement*)i->get();
            std::string name = e->getName();
            std::transform( name.begin(), name.end(), name.begin(), tolower );
            if ( name == name_lower )
                return e;
        }
    }
    return NULL;
}


std::string
XmlElement::getText() const
{
    std::stringstream builder;

    for( XmlNodeList::const_iterator i = getChildren().begin(); i != getChildren().end(); i++ )
    {
        if ( i->get()->isText() )
        {
            builder << ( static_cast<XmlText*>( i->get() ) )->getValue();
        }
    }

    std::string result = trim( builder.str() );
    return result;
}


std::string
XmlElement::getSubElementText( const std::string& name ) const
{
    XmlElement* e = getSubElement( name );
    return e? e->getText() : EMPTY_VALUE;
}


XmlNodeList 
XmlElement::getSubElements( const std::string& name ) const
{
    XmlNodeList results;

    std::string name_lower = name;
    std::transform( name_lower.begin(), name_lower.end(), name_lower.begin(), tolower );

    for( XmlNodeList::const_iterator i = getChildren().begin(); i != getChildren().end(); i++ )
    {
        if ( i->get()->isElement() )
        {
            XmlElement* e = (XmlElement*)i->get();
            std::string name = e->getName();
            std::transform( name.begin(), name.end(), name.begin(), tolower );
            if ( name == name_lower )
                results.push_back( e );
        }
    }

    return results;
}


XmlText::XmlText( const std::string& _value )
{
    value = _value;
}

const std::string&
XmlText::getValue() const
{
    return value;
}


XmlDocument::XmlDocument( const std::string& _source_uri ) :
XmlElement( "Document" ),
source_uri( _source_uri )
{
    //NOP
}

XmlDocument::~XmlDocument()
{
    //NOP
}


static XmlAttributes
getAttributes( const char** attrs )
{
    XmlAttributes map;
    const char** ptr = attrs;
    while( *ptr != NULL )
    {
        std::string name = *ptr++;
        std::string value = *ptr++;
        map[name] = value;
    }
    return map;
}


static void XMLCALL
startElement( void* user_data, const XML_Char* c_tag, const XML_Char** c_attrs )
{
    XmlElementNoRefStack& stack = *(XmlElementNoRefStack*)user_data;
    XmlElement* top = stack.top();

    std::string tag( c_tag );
    std::transform( tag.begin(), tag.end(), tag.begin(), tolower );
    XmlAttributes attrs = getAttributes( c_attrs );

    XmlElement* new_element = new XmlElement( tag, attrs );
    top->getChildren().push_back( new_element );
    stack.push( new_element );
}

static void XMLCALL
endElement( void* user_data, const XML_Char* c_tag )
{
    XmlElementNoRefStack& stack = *(XmlElementNoRefStack*)user_data;
    XmlElement* top = stack.top();
    stack.pop();
} 

static void XMLCALL
handleCharData( void* user_data, const XML_Char* c_data, int len )
{
    if ( len > 0 )
    {
        XmlElementNoRefStack& stack = *(XmlElementNoRefStack*)user_data;
        XmlElement* top = stack.top();
        std::string data( c_data, len );
        top->getChildren().push_back( new XmlText( data ) );
    }
}

XmlDocument*
XmlDocument::load( std::istream& in )
{
    XmlElementNoRefStack tree;

    XmlDocument* doc = new XmlDocument();
    tree.push( doc );

#define BUFSIZE 1024
    char buf[BUFSIZE];
    XML_Parser parser = XML_ParserCreate( NULL );
    bool done = false;
    XML_SetUserData( parser, &tree );
    XML_SetElementHandler( parser, startElement, endElement );
    XML_SetCharacterDataHandler( parser, (XML_CharacterDataHandler)handleCharData );
    while( !in.eof() )
    {
        in.read( buf, BUFSIZE );
        int bytes_read = in.gcount();
        if ( bytes_read > 0 )
        {
            if ( XML_Parse( parser, buf, bytes_read, in.eof() ) == XML_STATUS_ERROR )
            {
                osg::notify( osg::WARN ) 
                    << XML_ErrorString( XML_GetErrorCode( parser ) )
                    << ", "
                    << XML_GetCurrentLineNumber( parser ) 
                    << std::endl;

                XML_ParserFree( parser );
                return NULL;
            }
        }
    }
    XML_ParserFree( parser );
    return doc;
}

#define INDENT 4

static void
storeNode( XmlNode* node, int depth, std::ostream& out )
{
    for( int k=0; k<depth*INDENT; k++ )
        out << " ";

    if ( node->isElement() )
    {
        XmlElement* e = (XmlElement*)node;
        out << "<" << e->getName();
        for( XmlAttributes::iterator a = e->getAttrs().begin(); a != e->getAttrs().end(); a++ )
        {
            out << " " << a->first << "=" << "\"" << a->second << "\"";
        }
        out << ">" << std::endl;
        for( XmlNodeList::iterator i = e->getChildren().begin(); i != e->getChildren().end(); i++ )
        {
            storeNode( i->get(), depth+1, out );
        }
    }
    else if ( node->isText() )
    {
        XmlText* t = (XmlText*)node;
        //out << t->getValue() << std::endl;
    }
}

void
XmlDocument::store( std::ostream& out ) const
{
    out << "<?xml version=\"1.0\"?>" << std::endl;
    for( XmlNodeList::const_iterator i = getChildren().begin(); i != getChildren().end(); i++ )
    {
        storeNode( i->get(), 0, out );
    }
}