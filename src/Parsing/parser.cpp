#include "parser.h"
#include "../ErrorHandling/errorHandler.h"
#include "../DebugPrinting/ASTPrinter.h"
#include "../Includes/fmt/format.h"
#include "../SemanticAnalysis/semanticAnalyzer.h"

using std::make_shared;
using namespace AST;

// Have to define this in the AST namespace because parselets are c++ friend classes
namespace AST {
    //!, -, ~, $, --, ++, async, await, .., ..=
    ASTNodePtr parsePrefix(Parser* parser, Token token) {
        switch (token.type) {
            // Macro meta variables
            case TokenType::DOLLAR:{
                Token metaVar = parser->consume(TokenType::IDENTIFIER, "Expected identifier after '$'.");
                return parser->exprMetaVars[metaVar.getLexeme()]->get();
            }
            case TokenType::AWAIT: {
                // Syntax is await <expr>
                ASTNodePtr expr = parser->expression();
                return make_shared<AwaitExpr>(token, expr);
            }
            case TokenType::DOUBLE_DOT:{
                if(!parser->prefixParselets.contains(parser->peek().type)) return make_shared<RangeExpr>(token, nullptr, nullptr, false);
                auto expr = parser->expression(+Precedence::RANGE);
                return make_shared<RangeExpr>(token, nullptr, expr, false);
            }
            case TokenType::DOUBLE_DOT_EQUAL:{
                auto expr = parser->expression(+Precedence::RANGE);
                return make_shared<RangeExpr>(token, nullptr, expr, true);
            }
            case TokenType::ASYNC: {
                ASTNodePtr expr = parser->expression();
                if (expr->type != ASTType::CALL) throw parser->error(token, "Expected a call after 'async'.");
                auto call = std::static_pointer_cast<CallExpr>(expr);
                return make_shared<AsyncExpr>(token, call->callee, call->args);
            }
            default: {
                ASTNodePtr expr = parser->expression(parser->prefixPrecLevel(token.type));
                return make_shared<UnaryExpr>(token, expr, true);
            }
        }
    }

    ASTNodePtr parseLiteral(Parser* parser, Token token) {
        switch (token.type) {
            // Super is always followed by a .
            case TokenType::SUPER: {
                parser->consume(TokenType::DOT, "Expected '.' after super.");
                Token ident = parser->consume(TokenType::IDENTIFIER, "Expect superclass method name.");
                return make_shared<SuperExpr>(ident);
            }
            case TokenType::LEFT_PAREN: {
                // Grouping can contain an expr of any precedence
                ASTNodePtr expr = parser->expression();
                parser->consume(TokenType::RIGHT_PAREN, "Expected ')' at the end of grouping expression.");
                return expr;
            }
                // Array literal
            case TokenType::LEFT_BRACKET: {
                vector<ASTNodePtr> members;
                if (parser->peek().type != TokenType::RIGHT_BRACKET) {
                    do {
                        members.push_back(parser->expression());
                    } while (parser->match(TokenType::COMMA));
                }
                parser->consume(TokenType::RIGHT_BRACKET, "Expect ']' at the end of an array literal.");
                return make_shared<ArrayLiteralExpr>(members);
            }
                // Struct literal
            case TokenType::LEFT_BRACE: {
                vector<StructEntry> entries;
                if (parser->peek().type != TokenType::RIGHT_BRACE) {
                    // A struct literal looks like this: {var1 : expr1, var2 : expr2}
                    do {
                        Token identifier = parser->consume(TokenType::STRING, "Expected a string identifier.");
                        parser->consume(TokenType::COLON, "Expected a ':' after string identifier");
                        ASTNodePtr expr = parser->expression();
                        entries.emplace_back(identifier, expr);
                    } while (parser->match(TokenType::COMMA));
                }
                parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after struct literal.");
                return make_shared<StructLiteral>(entries);
            }
                // Function literal
            case TokenType::FN: {
                // The depths are used for throwing errors for switch and loops stmts,
                // and since a function can be declared inside a loop we need to account for that
                int tempLoopDepth = parser->loopDepth;
                int tempSwitchDepth = parser->switchDepth;
                parser->loopDepth = 0;
                parser->switchDepth = 0;

                parser->consume(TokenType::LEFT_PAREN, "Expect '(' for arguments.");
                vector<ASTVar> args;
                if (!parser->check(TokenType::RIGHT_PAREN)) {
                    do {
                        Token arg = parser->consume(TokenType::IDENTIFIER, "Expect argument name");
                        args.emplace_back(arg);
                        if (args.size() > 127) {
                            throw parser->error(arg, "Functions can't have more than 128 arguments");
                        }
                    } while (parser->match(TokenType::COMMA));
                }
                parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after arguments");
                parser->consume(TokenType::LEFT_BRACE, "Expect '{' after arguments.");
                shared_ptr<BlockStmt> body = parser->blockStmt();

                parser->loopDepth = tempLoopDepth;
                parser->switchDepth = tempSwitchDepth;
                return make_shared<FuncLiteral>(args, body);
            }
            case TokenType::NEW:{
                // new keyword is followed by a call to the class that is being instantiated, class must be an identifier
                // or module access to identifier
                auto call = parser->expression(+Precedence::CALL - 1);
                if(call->type != ASTType::CALL) throw parser->error(token, "Expected a call to class.");
                auto castCall = std::static_pointer_cast<CallExpr>(call);
                auto type = castCall->callee->type;
                if(!(type == AST::ASTType::LITERAL || type == AST::ASTType::MODULE_ACCESS)) {
                    throw parser->error(token, "Expected a class identifier or module access to class identifier.");
                }
                return make_shared<NewExpr>(castCall, token);
            }
                //number, string, boolean or nil
            default:
                return make_shared<LiteralExpr>(token);
        }
    }

    // Parses =, +=, -=, *=, /=, %=, ^=, |=, &=
    static ASTNodePtr parseAssign(ASTNodePtr left, Token op, ASTNodePtr right) {
        // No token other than the ones listed here will ever be passed to parseAssign
        switch (op.type) {
            case TokenType::EQUAL: {
                break;
            }
            case TokenType::PLUS_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::PLUS, "+"), right);
                break;
            }
            case TokenType::MINUS_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::MINUS, "-"), right);
                break;
            }
            case TokenType::SLASH_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::SLASH, "/"), right);
                break;
            }
            case TokenType::STAR_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::STAR, "*"), right);
                break;
            }
            case TokenType::BITWISE_XOR_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::BITWISE_XOR, "^"), right);
                break;
            }
            case TokenType::BITWISE_AND_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::BITWISE_AND, "&"), right);
                break;
            }
            case TokenType::BITWISE_OR_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::BITWISE_OR, "|"), right);
                break;
            }
            case TokenType::PERCENTAGE_EQUAL: {
                right = make_shared<BinaryExpr>(left, Token(TokenType::PERCENTAGE, "%"), right);
                break;
            }
        }
        return right;
    }
    ASTNodePtr parseAssignment(Parser* parser, ASTNodePtr left, Token token) {
        if (!(left->type == ASTType::LITERAL || left->type == ASTType::FIELD_ACCESS)) throw parser->error(token, "Left side is not assignable");
        // Precedence level -1 makes assignment right to left associative since parser->expression call won't stop when it hits '=' token
        // E.g. a = b = 2; gets parsed as a = (b = 2);
        auto rhs = parser->expression(parser->infixPrecLevel(token.type) - 1);
        rhs = parseAssign(left, token, rhs);
        // Assignment can be either variable assignment or set expression
        if(left->type == ASTType::LITERAL){
            left->accept(parser->probe);
            Token temp = parser->probe->getProbedToken();
            if (temp.type != TokenType::IDENTIFIER) throw parser->error(token, "Left side is not assignable");
            return make_shared<AssignmentExpr>(temp, rhs);
        }
        // Set expr, e.g. a.b = 3;
        auto fieldAccess = std::static_pointer_cast<FieldAccessExpr>(left);
        return make_shared<SetExpr>(fieldAccess->callee, fieldAccess->field, fieldAccess->accessor, rhs);
    }

    //?: operator
    ASTNodePtr parseConditional(Parser* parser, ASTNodePtr left, Token token){
        ASTNodePtr mhs = parser->expression();
        parser->consume(TokenType::COLON, "Expected ':' after then branch.");
        //Makes conditional right to left associative
        // a ? b : c ? d : e gets parsed as a ? b : (c ? d : e)
        ASTNodePtr rhs = parser->expression(+Precedence::CONDITIONAL - 1);
        return make_shared<ConditionalExpr>(left, mhs, rhs);
    }

    // Binary ops, module access(::) and macro invocation(!)
    static bool isComparisonOp(Token token){
        auto t = token.type;
        return (t == TokenType::EQUAL_EQUAL || t == TokenType::BANG_EQUAL ||
                t == TokenType::LESS || t == TokenType::LESS_EQUAL ||
                t == TokenType::GREATER || t== TokenType::GREATER_EQUAL);
    }
    ASTNodePtr parseBinary(Parser* parser, ASTNodePtr left, Token token){
        switch(token.type){
            // Module access cannot be chained, so it throws an error if left side isn't an identifier
            case TokenType::DOUBLE_COLON:{
                if(left->type != ASTType::LITERAL) throw parser->error(token, "Expected left side to be a module name.");
                left->accept(parser->probe);
                Token lhs = parser->probe->getProbedToken();
                if(lhs.type != TokenType::IDENTIFIER) throw parser->error(lhs, "Expected identifier for module name.");
                Token ident = parser->consume(TokenType::IDENTIFIER, "Expected variable name.");
                return make_shared<ModuleAccessExpr>(lhs, ident);
            }
            case TokenType::BANG:{
                if(left->type != ASTType::LITERAL) throw parser->error(token, "Expected macro name to be an identifier.");
                left->accept(parser->probe);
                Token macroName = parser->probe->getProbedToken();
                if (macroName.type != TokenType::IDENTIFIER) {
                    throw parser->error(macroName, "Expected macro name to be an identifier.");
                }
                if (!parser->macros.contains(macroName.getLexeme())) {
                    throw parser->error(macroName, "Invoked macro isn't defined");
                }
                return make_shared<MacroExpr>(macroName, parser->readTokenTree());
            }
            case TokenType::INSTANCEOF:{
                auto right = parser->expression(parser->infixPrecLevel(token.type));
                if(!(right->type == ASTType::LITERAL || right->type == ASTType::MODULE_ACCESS)){
                    throw parser->error(token, "Right side of the 'instanceof' operator can only be an identifier.");
                }
                return make_shared<BinaryExpr>(left, token, right);
            }
            default:{
                ASTNodePtr right = parser->expression(parser->infixPrecLevel(token.type));
                if(!isComparisonOp(token)) return make_shared<BinaryExpr>(left, token, right);

                // Chaining comparison ops is forbidden, here lhs is checked against op of this binary expr,
                // After parsing rhs, rhs is compared to op of this binary expr
                if(left->type == ASTType::BINARY){
                    auto op = std::static_pointer_cast<BinaryExpr>(left)->op;
                    if(isComparisonOp(op)){
                        parser->error(op, "Cannot chain comparison operators.");
                        parser->error(token, "Second comparison operator here.");
                    }
                }
                if(right->type == ASTType::BINARY){
                    auto op = std::static_pointer_cast<BinaryExpr>(right)->op;
                    if(isComparisonOp(op)){
                        parser->error(token, "Second comparison operator here.");
                        parser->error(op, "Cannot chain comparison operators.");
                    }
                }
                return make_shared<BinaryExpr>(left, token, right);
            }
        }
    }

    ASTNodePtr parsePostfix(Parser* parser, ASTNodePtr left, Token token){
        switch(token.type){
            // Handles both infix and postfix range ops
            case TokenType::DOUBLE_DOT_EQUAL:{
                if(+Precedence::RANGE < parser->prefixPrecLevel(parser->peek().type)){
                    auto expr = parser->expression(parser->infixPrecLevel(token.type));
                    return make_shared<RangeExpr>(token, left, expr, true);
                }
                throw parser->error(token, "End inclusive range operator used without end of range.");
            }
            case TokenType::DOUBLE_DOT:{
                if(+Precedence::RANGE < parser->prefixPrecLevel(parser->peek().type)){
                    auto expr = parser->expression(parser->infixPrecLevel(token.type));
                    return make_shared<RangeExpr>(token, left, expr, false);
                }
                return make_shared<RangeExpr>(token, left, nullptr, false);
            }
            default: return make_shared<UnaryExpr>(token, left, false);
        }
    }

    ASTNodePtr parseCall(Parser* parser, ASTNodePtr left, Token token){
        vector<ASTNodePtr> args;
        if (!parser->check(TokenType::RIGHT_PAREN)) {
            do {
                args.push_back(parser->expression());
            } while (parser->match(TokenType::COMMA));
        }
        parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after call expression.");
        return make_shared<CallExpr>(left, args);
    }

    ASTNodePtr parseFieldAccess(Parser* parser, ASTNodePtr left, Token token){
        ASTNodePtr field = nullptr;
        if (token.type == TokenType::LEFT_BRACKET) {// Array/struct with string access
            field = parser->expression();
            parser->consume(TokenType::RIGHT_BRACKET, "Expect ']' after array/map access.");
        }
        else if (token.type == TokenType::DOT) {// Object access
            Token fieldName = parser->consume(TokenType::IDENTIFIER, "Expected a field identifier.");
            field = make_shared<LiteralExpr>(fieldName);
        }
        return make_shared<FieldAccessExpr>(left, token, field);
    }
}

Parser::Parser() {
    probe = new ASTProbe;
    macroExpander = new MacroExpander(this);

    loopDepth = 0;
    switchDepth = 0;
    parsedUnit = nullptr;

    currentContainer = nullptr;
    currentPtr = 0;

#pragma region Parselets
    // Prefix
    addPrefix(TokenType::DOUBLE_DOT, Precedence::RANGE, parsePrefix);
    addPrefix(TokenType::DOUBLE_DOT_EQUAL, Precedence::RANGE, parsePrefix);

    addPrefix(TokenType::BANG, Precedence::UNARY_PREFIX, parsePrefix);
    addPrefix(TokenType::MINUS, Precedence::UNARY_PREFIX, parsePrefix);
    addPrefix(TokenType::TILDA, Precedence::UNARY_PREFIX, parsePrefix);
    // Only for macros
    addPrefix(TokenType::DOLLAR, Precedence::UNARY_PREFIX, parsePrefix);

    addPrefix(TokenType::INCREMENT, Precedence::UNARY_PREFIX, parsePrefix);
    addPrefix(TokenType::DECREMENT, Precedence::UNARY_PREFIX, parsePrefix);

    addPrefix(TokenType::IDENTIFIER, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::STRING, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::NUMBER, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::TRUE, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::FALSE, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::NIL, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::LEFT_PAREN, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::LEFT_BRACKET, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::LEFT_BRACE, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::SUPER, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::FN, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::NEW, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::THIS, Precedence::PRIMARY, parseLiteral);
    addPrefix(TokenType::ASYNC, Precedence::PRIMARY, parsePrefix);
    addPrefix(TokenType::AWAIT, Precedence::PRIMARY, parsePrefix);

    // Infix and mix-fix
    addInfix(TokenType::EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::PLUS_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::MINUS_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::SLASH_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::STAR_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::PERCENTAGE_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::BITWISE_XOR_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::BITWISE_OR_EQUAL, Precedence::ASSIGNMENT, parseAssignment);
    addInfix(TokenType::BITWISE_AND_EQUAL, Precedence::ASSIGNMENT, parseAssignment);

    addInfix(TokenType::QUESTIONMARK, Precedence::CONDITIONAL, parseConditional);

    addInfix(TokenType::OR, Precedence::OR, parseBinary);
    addInfix(TokenType::AND, Precedence::AND, parseBinary);

    addInfix(TokenType::BITWISE_OR, Precedence::BIN_OR, parseBinary);
    addInfix(TokenType::BITWISE_XOR, Precedence::BIN_XOR, parseBinary);
    addInfix(TokenType::BITWISE_AND, Precedence::BIN_AND, parseBinary);

    addInfix(TokenType::IN, Precedence::COMPARISON, parseBinary);

    addInfix(TokenType::EQUAL_EQUAL, Precedence::COMPARISON, parseBinary);
    addInfix(TokenType::BANG_EQUAL, Precedence::COMPARISON, parseBinary);

    addInfix(TokenType::LESS, Precedence::COMPARISON, parseBinary);
    addInfix(TokenType::LESS_EQUAL, Precedence::COMPARISON, parseBinary);
    addInfix(TokenType::GREATER, Precedence::COMPARISON, parseBinary);
    addInfix(TokenType::GREATER_EQUAL, Precedence::COMPARISON, parseBinary);

    addInfix(TokenType::BITSHIFT_LEFT, Precedence::BITSHIFT, parseBinary);
    addInfix(TokenType::BITSHIFT_RIGHT, Precedence::BITSHIFT, parseBinary);

    addInfix(TokenType::PLUS, Precedence::SUM, parseBinary);
    addInfix(TokenType::MINUS, Precedence::SUM, parseBinary);

    addInfix(TokenType::SLASH, Precedence::FACTOR, parseBinary);
    addInfix(TokenType::STAR, Precedence::FACTOR, parseBinary);
    addInfix(TokenType::PERCENTAGE, Precedence::FACTOR, parseBinary);
    addInfix(TokenType::BANG, Precedence::PRIMARY, parseBinary);

    addInfix(TokenType::LEFT_PAREN, Precedence::CALL, parseCall);
    addInfix(TokenType::LEFT_BRACKET, Precedence::CALL, parseFieldAccess);
    addInfix(TokenType::DOT, Precedence::CALL, parseFieldAccess);
    addInfix(TokenType::INSTANCEOF, Precedence::INSTANCEOF, parseBinary);

    addInfix(TokenType::DOUBLE_COLON, Precedence::PRIMARY, parseBinary);

    // Postfix
    addPostfix(TokenType::DOUBLE_DOT, Precedence::RANGE, parsePostfix);
    addPostfix(TokenType::DOUBLE_DOT_EQUAL, Precedence::RANGE, parsePostfix);

    addPostfix(TokenType::INCREMENT, Precedence::UNARY_POSTFIX, parsePostfix);
    addPostfix(TokenType::DECREMENT, Precedence::UNARY_POSTFIX, parsePostfix);
#pragma endregion
}

void Parser::parse(vector<CSLModule*>& modules) {
#ifdef AST_DEBUG
    ASTPrinter* astPrinter = new ASTPrinter;
#endif
    // Modules are already sorted using topsort
    for (CSLModule* unit : modules) {
        parsedUnit = unit;

        // Parse tokenized source into AST
        loopDepth = 0;
        switchDepth = 0;
        currentContainer = &parsedUnit->tokens;
        currentPtr = 0;
        while (!isAtEnd()) {
            try {
                if (match(TokenType::ADDMACRO)) {
                    defineMacro();
                    continue;
                }
                unit->stmts.push_back(topLevelDeclaration());
#ifdef AST_DEBUG
                //prints statement
				unit->stmts[unit->stmts.size() - 1]->accept(astPrinter);
#endif
            }
            catch (ParserException& e) {
                sync();
            }
        }

        expandMacros();
    }
    // 2 units being imported using the same alias is illegal
    // Units imported without an alias must abide by the rule that every symbol must be unique
    for (CSLModule* unit : modules) {
        std::unordered_map<string, Dependency*> symbols;
        // Symbols of this unit are also taken into account when checking uniqueness
        for(auto decl : unit->topDeclarations){
            symbols[decl->getName().getLexeme()] = nullptr;
        }
        std::unordered_map<string, Dependency*> importAliases;

        for (Dependency& dep : unit->deps) {
            if (dep.alias.type == TokenType::NONE) {
                for (const auto decl : dep.module->exports) {
                    string lexeme = decl->getName().getLexeme();

                    if (symbols.count(lexeme) == 0) {
                        symbols[lexeme] = &dep;
                        continue;
                    }
                    // If there are 2 or more declaration which use the same symbol,
                    // throw an error and tell the user exactly which dependencies caused the error

                    string str = fmt::format("Ambiguous definition, symbol '{}' defined in {} and {}.",
                                             lexeme, symbols[lexeme] ? symbols[lexeme]->pathString.getLexeme() : "this file", dep.pathString.getLexeme());
                    if(!symbols[lexeme]){
                        for(auto thisFileDecl : unit->topDeclarations){
                            if(thisFileDecl->getName().getLexeme() != lexeme) continue;
                            error(thisFileDecl->getName(), str);
                        }
                    }else error(dep.pathString, str);
                }
            }
            else {
                // Check if any imported dependencies share the same alias
                if (importAliases.count(dep.alias.getLexeme()) > 0) {
                    error(importAliases[dep.alias.getLexeme()]->alias, "Cannot use the same alias for 2 module imports.");
                    error(dep.alias, "Cannot use the same alias for 2 module imports.");
                }
                importAliases[dep.alias.getLexeme()] = &dep;
            }
        }
    }
}

void Parser::highlight(vector<CSLModule*>& modules, string moduleToHighlight){
    // Modules are already sorted using topsort
    for (CSLModule* unit : modules) {
        parsedUnit = unit;

        // Parse tokenized source into AST
        loopDepth = 0;
        switchDepth = 0;
        currentContainer = &parsedUnit->tokens;
        currentPtr = 0;
        while (!isAtEnd()) {
            try {
                if (match(TokenType::ADDMACRO)) {
                    defineMacro();
                    continue;
                }
                unit->stmts.push_back(topLevelDeclaration());
            }
            catch (ParserException& e) {
                sync();
            }
        }
        if(unit->file->path == moduleToHighlight){
            SemanticAnalysis::SemanticAnalyzer semanticAnalyzer;
            std::cout << semanticAnalyzer.highlight(modules, unit, macros);
            return;
        }
        expandMacros();
    }
}

void Parser::defineMacro() {
    consume(TokenType::BANG, "Expected '!' after 'addMacro' token.");
    Token macroName = consume(TokenType::IDENTIFIER, "Expected macro name to be an identifier.");
    consume(TokenType::LEFT_BRACE, "Expected '{' initiating macro definition.");

    macros[macroName.getLexeme()] = std::make_unique<Macro>(macroName, this);
    auto& macro = macros[macroName.getLexeme()];

    while (!isAtEnd() && !check(TokenType::RIGHT_BRACE)) {
        MatchPattern matcher(readTokenTree(), this);
        consume(TokenType::ARROW, "Expected '=>' after matcher expression.");
        if (!check(TokenType::LEFT_BRACE)) { throw error(peek(), "Expected '{' initiating transcriber expression."); }
        vector<Token> transcriber = readTokenTree();
        // erase '{' and '}' from transcriber
        transcriber.erase(transcriber.begin());
        transcriber.pop_back();

        consume(TokenType::SEMICOLON, "Expected ';' after transcriber expression.");

        macro->matchers.push_back(matcher);
        macro->transcribers.push_back(transcriber);
    }
    consume(TokenType::RIGHT_BRACE, "Unexpected incomplete macro definition.");
}

ASTNodePtr Parser::expression(int prec) {
    Token token = advance();
    //check if the token has a prefix function associated with it, and if it does, parse with it
    if (prefixParselets.count(token.type) == 0) {
        // TODO: Fix hackyness
        if (token.str.length == 0) token = currentContainer->at(currentPtr - 2);
        throw error(token, "Expected expression.");
    }
    if (token.type == TokenType::DOLLAR && parseMode != ParseMode::Macro){
        throw error(token, "Unexpected '$' found outside of macro transcriber.");
    }
    auto& prefix = prefixParselets[token.type];
    shared_ptr<ASTNode> left = prefix.second(this, token);


    while(true){
        //Postfix
        if (postfixParselets.contains(peek().type) && prec < postfixPrecLevel(peek().type)){
            token = advance();
            auto& postfix = postfixParselets[token.type];
            left = postfix.second(this, left, token);
            continue;
        }

        //advances only if the next token has a higher precedence than the parserCurrent one
        //e.g. 1 + 2 compiles because the base precedence is 0, and '+' has a precedence of 11
        //loop runs as long as the next operator has a higher precedence than the one that called this function
        if(infixParselets.contains(peek().type) && prec < infixPrecLevel(peek().type)){
            token = advance();
            if (infixParselets.count(token.type) == 0) {
                throw error(token, "Expected expression.");
            }
            auto& infix = infixParselets[token.type];
            left = infix.second(this, left, token);
            continue;
        }
        break;
    }
    return left;
}

ASTNodePtr Parser::expression() {
    return expression(0);
}

#pragma region Statements and declarations
//module level variables are put in a list to help with error reporting in compiler
ASTNodePtr Parser::topLevelDeclaration() {
    //export is only allowed in global scope
    shared_ptr<ASTDecl> node = nullptr;
    bool isExported = false;
    if (match(TokenType::PUB)) isExported = true;
    if (match(TokenType::LET)) node = varDecl();
    else if (match(TokenType::CLASS)) node = classDecl();
    else if (match(TokenType::FN)) node = funcDecl();
    else if(isExported) throw error(previous(), "Only declarations are allowed after 'export'");
    if(node){
        for(const auto decl : parsedUnit->topDeclarations){
            if (node->getName().equals(decl->getName())) {
                error(node->getName(), fmt::format("Error, {} already defined.", node->getName().getLexeme()));
                throw error(decl->getName(), fmt::format("Error, redefinition of {}.", decl->getName().getLexeme()));
            }
        }
        // Passing in the actual AST node, not just the name, because it also contains info about declaration type(var, func, class)
        parsedUnit->topDeclarations.push_back(node);
        if(isExported) parsedUnit->exports.push_back(node);
        return node;
    }
    return statement();
}

ASTNodePtr Parser::localDeclaration() {
    if (match(TokenType::LET)) return varDecl();
    return statement();
}

shared_ptr<VarDecl> Parser::varDecl() {
    Token name = consume(TokenType::IDENTIFIER, "Expected a variable identifier.");
    ASTNodePtr expr = nullptr;
    //if no initializer is present the variable is initialized to null
    if (match(TokenType::EQUAL)) {
        expr = expression();
    }
    consume(TokenType::SEMICOLON, "Expected a ';' after variable declaration.");
    return make_shared<VarDecl>(name, expr);
}

shared_ptr<FuncDecl> Parser::funcDecl() {
    //the depths are used for throwing errors for switch and loops stmts,
    //and since a function can be declared inside a loop we need to account for that
    int tempLoopDepth = loopDepth;
    int tempSwitchDepth = switchDepth;
    loopDepth = 0;
    switchDepth = 0;

    Token name = consume(TokenType::IDENTIFIER, "Expected a function name.");
    consume(TokenType::LEFT_PAREN, "Expect '(' after function name.");
    vector<ASTVar> args;
    //parse args
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            Token arg = consume(TokenType::IDENTIFIER, "Expect argument name");
            args.emplace_back(arg);
            if (args.size() > 127) {
                throw error(arg, "Functions can't have more than 127 arguments");
            }
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RIGHT_PAREN, "Expect ')' after arguments");
    consume(TokenType::LEFT_BRACE, "Expect '{' after arguments.");
    shared_ptr<BlockStmt> body = blockStmt();

    loopDepth = tempLoopDepth;
    switchDepth = tempSwitchDepth;
    return make_shared<FuncDecl>(name, args, body);
}

shared_ptr<ClassDecl> Parser::classDecl() {
    Token name = consume(TokenType::IDENTIFIER, "Expected a class name.");
    ASTNodePtr inherited = nullptr;
    // Inheritance is optional
    if (match(TokenType::COLON)) {
        Token token = previous();
        // Only accept identifiers and module access
        inherited = expression(+Precedence::PRIMARY - 1);
        if (!((inherited->type == ASTType::LITERAL && dynamic_cast<LiteralExpr*>(inherited.get())->token.type == TokenType::IDENTIFIER)
              || inherited->type == ASTType::MODULE_ACCESS)) {
            error(token, "Superclass can only be an identifier.");
        }
    }

    consume(TokenType::LEFT_BRACE, "Expect '{' before class body.");

    vector<ClassMethod> methods;
    vector<ClassField> fields;

    auto checkName = [&](Token token){
        for(auto& m : methods){
            if(token.equals(m.method->name)) {
                error(token, "Re-declaration of method.");
                throw error(m.method->name, "Method first defined here.");
            }
        }
        for(auto& field : fields){
            if(token.equals(field.field)) {
                error(token, "Re-declaration of field.");
                throw error(field.field, "Field first defined here.");
            }
        }
    };

    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        try {
            bool isPublic = false;
            if (match(TokenType::PUB)) {
                isPublic = true;
            }
            if (match(TokenType::LET)) {
                Token field = consume(TokenType::IDENTIFIER, "Expected a field identifier.");

                checkName(field);
                fields.emplace_back(isPublic, field);

                while (!check(TokenType::SEMICOLON) && !check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                    if (!match(TokenType::COMMA)) break;
                    field = consume(TokenType::IDENTIFIER, "Expected a field identifier.");
                    checkName(field);
                    fields.emplace_back(isPublic, field);
                }

                consume(TokenType::SEMICOLON, "Expected ';' after field name");
            } else if (match(TokenType::FN)) {
                auto decl = funcDecl();
                checkName(decl->name);
                // Implicitly declare "this"
                decl->args.insert(decl->args.begin(), ASTVar(Token(TokenType::IDENTIFIER, "this")));
                methods.emplace_back(isPublic, decl);
            } else {
                throw error(peek(), "Expected let or fn keywords.");
            }
        }catch(ParserException& e){
            sync();
        }
    }
    consume(TokenType::RIGHT_BRACE, "Expect '}' after class body.");
    return make_shared<ClassDecl>(name, methods, fields, inherited);
}

ASTNodePtr Parser::statement() {
    if (match({ TokenType::LEFT_BRACE, TokenType::IF, TokenType::WHILE,
                TokenType::FOR, TokenType::BREAK, TokenType::SWITCH,
                TokenType::RETURN, TokenType::CONTINUE, TokenType::ADVANCE })) {

        switch (previous().type) {
            case TokenType::LEFT_BRACE: return blockStmt();
            case TokenType::IF: return ifStmt();
            case TokenType::WHILE: return whileStmt();
            case TokenType::FOR: return forStmt();
            case TokenType::BREAK: return breakStmt();
            case TokenType::CONTINUE: return continueStmt();
            case TokenType::ADVANCE: return advanceStmt();
            case TokenType::SWITCH: return switchStmt();
            case TokenType::RETURN: return returnStmt();
        }
    }
    return exprStmt();
}

shared_ptr<ExprStmt> Parser::exprStmt() {
    ASTNodePtr expr = expression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression.");
    return make_shared<ExprStmt>(expr);
}

shared_ptr<BlockStmt> Parser::blockStmt() {
    vector<ASTNodePtr> stmts;
    //TokenType::LEFT_BRACE is already consumed
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        try {
            stmts.push_back(localDeclaration());
        }catch(ParserException& e){
            sync();
        }
    }
    consume(TokenType::RIGHT_BRACE, "Expect '}' after block.");
    return make_shared<BlockStmt>(stmts);
}

shared_ptr<IfStmt> Parser::ifStmt() {
    consume(TokenType::LEFT_PAREN, "Expect '(' after 'if'.");
    ASTNodePtr condition = expression();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");
    //using statement() instead of declaration() disallows declarations directly in a control flow body
    //declarations are still allowed in block statement
    ASTNodePtr thenBranch = statement();
    ASTNodePtr elseBranch = nullptr;
    if (match(TokenType::ELSE)) {
        elseBranch = statement();
    }
    return make_shared<IfStmt>(thenBranch, elseBranch, condition);
}

shared_ptr<WhileStmt> Parser::whileStmt() {
    //loopDepth is used to see if a 'continue' or 'break' statement is allowed within the body
    loopDepth++;
    consume(TokenType::LEFT_PAREN, "Expect '(' after 'while'.");
    ASTNodePtr condition = expression();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");
    ASTNodePtr body = statement();
    loopDepth--;
    return make_shared<WhileStmt>(body, condition);
}

shared_ptr<ForStmt> Parser::forStmt() {
    loopDepth++;
    consume(TokenType::LEFT_PAREN, "Expect '(' after 'for'.");
    //initializer can either be: empty, a new variable declaration, or any expression
    ASTNodePtr init = nullptr;
    if (match(TokenType::SEMICOLON)) {
        //do nothing
    }
    else if (match(TokenType::LET)) init = varDecl();
    else init = exprStmt();

    ASTNodePtr condition = nullptr;
    //we don't want to use exprStmt() because it emits OP_POP, and we'll need the value to determine whether to jump
    if (!check(TokenType::SEMICOLON)) condition = expression();
    consume(TokenType::SEMICOLON, "Expect ';' after loop condition");

    ASTNodePtr increment = nullptr;
    //using expression() here instead of exprStmt() because there is no trailing ';'
    if (!check(TokenType::RIGHT_PAREN)) increment = expression();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after 'for' clauses.");
    //disallows declarations unless they're in a block
    ASTNodePtr body = statement();
    loopDepth--;
    return make_shared<ForStmt>(init, condition, increment, body);
}

shared_ptr<BreakStmt> Parser::breakStmt() {
    if (loopDepth == 0 && switchDepth == 0) throw error(previous(), "Cannot use 'break' outside of loops or switch statements.");
    consume(TokenType::SEMICOLON, "Expect ';' after break.");
    return make_shared<BreakStmt>(previous());
}

shared_ptr<ContinueStmt> Parser::continueStmt() {
    if (loopDepth == 0) throw error(previous(), "Cannot use 'continue' outside of loops.");
    consume(TokenType::SEMICOLON, "Expect ';' after continue.");
    return make_shared<ContinueStmt>(previous());
}

shared_ptr<SwitchStmt> Parser::switchStmt() {
    //structure:
    //switch(<expression>){
    //case <expression>: <statements>
    //}
    consume(TokenType::LEFT_PAREN, "Expect '(' after 'switch'.");
    ASTNodePtr expr = expression();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after expression.");
    consume(TokenType::LEFT_BRACE, "Expect '{' after switch expression.");
    switchDepth++;
    vector<shared_ptr<CaseStmt>> cases;
    bool hasDefault = false;

    while (!check(TokenType::RIGHT_BRACE) && match({ TokenType::CASE, TokenType::DEFAULT })) {
        Token prev = previous();//to see if it's a default statement
        shared_ptr<CaseStmt> curCase = caseStmt();
        curCase->caseType = prev;
        if (prev.type == TokenType::DEFAULT) {
            //don't throw, it isn't a breaking error
            if (hasDefault) error(prev, "Only 1 default case is allowed inside a switch statement.");
            hasDefault = true;
        }
        cases.push_back(curCase);
    }
    consume(TokenType::RIGHT_BRACE, "Expect '}' after switch body.");
    switchDepth--;
    return make_shared<SwitchStmt>(expr, cases, hasDefault);
}

shared_ptr<CaseStmt> Parser::caseStmt() {
    vector<Token> matchConstants;
    //default cases don't have a match expression
    if (previous().type != TokenType::DEFAULT) {
        while (match({ TokenType::NIL, TokenType::NUMBER, TokenType::STRING, TokenType::TRUE, TokenType::FALSE })) {
            matchConstants.push_back(previous());
            if (!match(TokenType::BITWISE_OR)) break;
        }
        if (!match({ TokenType::NIL, TokenType::NUMBER, TokenType::STRING, TokenType::TRUE, TokenType::FALSE }) && peek().type != TokenType::COLON) {
            throw error(peek(), "Expression must be a constant literal(string, number, boolean or nil).");
        }
    }
    consume(TokenType::COLON, "Expect ':' after 'case' or 'default'.");
    vector<ASTNodePtr> stmts;
    while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !isAtEnd()) {
        try {
            stmts.push_back(localDeclaration());
        }catch(ParserException& e){
            sync();
        }
    }
    return make_shared<CaseStmt>(matchConstants, stmts);
}

shared_ptr<AdvanceStmt> Parser::advanceStmt() {
    if (switchDepth == 0) throw error(previous(), "Cannot use 'advance' outside of switch statements.");
    consume(TokenType::SEMICOLON, "Expect ';' after 'advance'.");
    return make_shared<AdvanceStmt>(previous());
}

shared_ptr<ReturnStmt> Parser::returnStmt() {
    ASTNodePtr expr = nullptr;
    Token keyword = previous();
    if (!match(TokenType::SEMICOLON)) {
        expr = expression();
        consume(TokenType::SEMICOLON, "Expect ';' at the end of 'return'.");
    }
    return make_shared<ReturnStmt>(expr, keyword);
}

#pragma endregion

#pragma region Helpers
//if the parserCurrent token type matches any of the provided tokenTypes it's consumed, if not false is returned
bool Parser::match(const std::initializer_list<TokenType>& tokenTypes) {
    if (check(tokenTypes)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(const TokenType type) {
    return match({ type });
}

bool Parser::isAtEnd() {
    return currentContainer->size() <= currentPtr;
}

bool Parser::check(const std::initializer_list<TokenType>& tokenTypes){
    if (isAtEnd()) return false;
    for (const TokenType& type : tokenTypes){
        if (type == peek().type){
            return true;
        }
    }
    return false;
}

bool Parser::check(const TokenType type) {
    return check({ type });
}

//returns parserCurrent token and increments to the next one
Token Parser::advance() {
    if (isAtEnd()) throw error(currentContainer->back(), "Expected token.");
    currentPtr++;
    return previous();
}

//gets parserCurrent token
Token Parser::peek() {
    if (isAtEnd()) throw error(currentContainer->back(), "Expected token.");
    return currentContainer->at(currentPtr);
}

//gets next token
Token Parser::peekNext() {
    if (currentContainer->size() <= currentPtr + 1) throw error(currentContainer->back(), "Expected token.");
    return currentContainer->at(currentPtr + 1);
}

Token Parser::previous() {
    if (currentPtr - 1 < 0) throw error(currentContainer->at(0), "Expected token.");
    return currentContainer->at(currentPtr - 1);
}

//if the parserCurrent token is of the correct type, it's consumed, if not an error is thrown
Token Parser::consume(TokenType type, string msg) {
    if (check(type)) return advance();

    throw error(peek(), msg);
}

ParserException Parser::error(Token token, string msg) {
    if (parseMode != ParseMode::Matcher) {
        errorHandler::addCompileError(msg, token);
    }
    return ParserException();
}

vector<Token> Parser::readTokenTree(bool isNonLeaf)
{
    if (!isNonLeaf && !check({TokenType::LEFT_PAREN, TokenType::LEFT_BRACE, TokenType::LEFT_BRACKET, TokenType::RIGHT_PAREN, TokenType::RIGHT_BRACE, TokenType::RIGHT_BRACKET})) { return { advance() }; }

    if (!check({TokenType::LEFT_PAREN, TokenType::LEFT_BRACE, TokenType::LEFT_BRACKET})) { throw error(peek(), "Expected '(', '{' or '[' initiating token tree."); }

    vector<Token> tokenTree;
    vector<TokenType> closerStack;

    do {
        if (isAtEnd()) {
            throw error(previous(), "Unexpected end of file.");
        }

        // Update closers
        if (check({TokenType::LEFT_PAREN, TokenType::LEFT_BRACE, TokenType::LEFT_BRACKET})) { closerStack.push_back(peek().type); }
        if (check(TokenType::RIGHT_PAREN)) {
            if (closerStack.back() != TokenType::LEFT_PAREN) { throw error(peek(), "Unexpected ')' in token tree."); }
            closerStack.pop_back();
        }
        if (check(TokenType::RIGHT_BRACE)){
            if (closerStack.back() != TokenType::LEFT_BRACE) { throw error(peek(), "Unexpected '}' in token tree."); }
            closerStack.pop_back();
        }
        if (check(TokenType::RIGHT_BRACKET)){
            if (closerStack.back() != TokenType::LEFT_BRACKET) { throw error(peek(), "Unexpected ']' in token tree."); }
            closerStack.pop_back();
        }

        tokenTree.push_back(advance());
        tokenTree[tokenTree.size() - 1].isPartOfMacro = true;
    } while (!closerStack.empty());

    return tokenTree;
}

void Parser::expandMacros()
{
    for (ASTNodePtr stmt : parsedUnit->stmts) {
        try {
            macroExpander->expand(stmt);
        }
        catch (ParserException& e) {}
    }
}

//syncs when we find a ';' or one of the keywords
void Parser::sync() {
    while (!isAtEnd()) {
        if (peek().type == TokenType::SEMICOLON){
            advance();
            return;
        }

        switch (peek().type) {
            case TokenType::CLASS:
            case TokenType::FN:
            case TokenType::LET:
            case TokenType::FOR:
            case TokenType::IF:
            case TokenType::ELSE:
            case TokenType::WHILE:
            case TokenType::RETURN:
            case TokenType::SWITCH:
            case TokenType::CASE:
            case TokenType::DEFAULT:
            case TokenType::LEFT_BRACE:
            case TokenType::RIGHT_BRACE:
            case TokenType::STATIC:
            case TokenType::PUB:
                return;
            default: break;
        }

        advance();
    }
}

void Parser::addPrefix(TokenType type, Precedence prec, PrefixFunc func){
    prefixParselets[type] = std::pair(+prec, func);
}
void Parser::addInfix(TokenType type, Precedence prec, InfixFunc func){
    infixParselets[type] = std::pair(+prec, func);
}
void Parser::addPostfix(TokenType type, Precedence prec, InfixFunc func){
    postfixParselets[type] = std::pair(+prec, func);
}

int Parser::prefixPrecLevel(TokenType type){
    if(!prefixParselets.contains(type)) return +Precedence::NONE;
    return prefixParselets[type].first;
}
int Parser::infixPrecLevel(TokenType type){
    if(!infixParselets.contains(type)) return +Precedence::NONE;
    return infixParselets[type].first;
}
int Parser::postfixPrecLevel(TokenType type){
    if(!postfixParselets.contains(type)) return +Precedence::NONE;
    return postfixParselets[type].first;
}
#pragma endregion