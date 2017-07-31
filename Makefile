TARGET=tutorial

all: ${TARGET}.pdf

pdf: ${TARGET}.pdf

${TARGET}.pdf: cleantex ${TARGET}.tex
	latex ${TARGET}
#	bibtex ${TARGET}
	latex ${TARGET}
	latex ${TARGET}
	dvipdf ${TARGET}.dvi

html: index.html

index.html: ${TARGET}.html
	./cleanupHTML.pl -i ${TARGET}.html > index.html

tutorial.html: tutorial.tex ViennaRNA-homepage.cfg
	htlatex tutorial 'ViennaRNA-homepage, mathml,charset=utf8' ' -css, -cmozhtf, -cunihtf'

clean:  cleantex cleanhtml

cleantex:
	rm -f *.ps *.dvi *.aux *.toc *.idx *.ind *.ilg *.log *.out *.brf *.blg *.bbl ${TARGET}.pdf

cleanhtml:
	rm -f *.4ct *.4tc *.tmp *.lg *.xref ${TARGET}.css ${TARGET}.html index.html

##.PHONY: tutorial.html index.html
