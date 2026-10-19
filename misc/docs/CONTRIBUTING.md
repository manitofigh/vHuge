1. Please try and write code that matches the existing 
programming style throughout the project. I'm not 
trying to be a dictator, but it is a bit frustrating to 
read and follow through a codebase where the programming 
style shifts as you jump from function to function. 
It's really no different than reading a book where the 
font keeps changing every paragraph. This includes but 
is not limited to having functions' curly braces start 
and end on their own new lines, using i32/64, u8/16/32/64, 
bool, etc as types, rather than int/unsigned/etc, and much 
more, which will become visible by looking through the repo.

2. Do not be verbose with your comments or overuse them. 
If your code already is clear enough on what is going on, 
it's not really necessary to waste an extra line. LLMs 
specifically seem to LOVE doing this... So please, if you 
do use an LLM/agent, make it read this rule.

3. Please keep commit messages all lowercase, unless 
referring to an acronym/abbreviation/name. This might 
be a bit dictatorial, but I do like them more. 
Somehow, they look cleaner (at least to me).

4. Try to have a relevant tag for your commit when possible. 
For example "bug-fix: xyz", "feat: abc", etc.

5. Please, if something can be done in 10 lines, don't do it 
in 20 just because it might look more impressive or complex. 
To me, lean is elegant and beautiful. Unless an additional line 
or two would result in the same generated binary by the compiler, 
and would save the reader many minutes in understanding what's 
going on, I see no reason to make an implementation longer than 
it should be.

6. Reuse already-existing functions/methods as much as possible 
instead of re-inventing the wheel please. A quick fuzzy search 
throughout the repo might bring up exactly what you are looking 
for! E.g. `maccess`, `time_maccess`, `_lfence`/`_mfence`, etc.

7. Please please please test your changes :) Thanks!
