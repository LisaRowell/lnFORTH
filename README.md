# Departures from FigFORTH
### **'**
In FigFORTH **'** was defined to be **immediate** with is not compatible with later standards and is different from FORTH as described by *Starting FORTH*.
Additionally **'** returned a word's PFA while FORTH79 and later returned the CFA. *lnFORTH* returns the CFA.
### **[']**
It provide an immediate word with the functionality of **'**. This addition matches later standards.
### **NFA**
As part of changing **'** to return a word's Code Field, **NFA** was changed to expect the word's Code Field as its input.
### **(FIND)**
### **-FIND**
Both **(FIND)** and **-FIND** were changed to return CFAs instead of PFAs.
