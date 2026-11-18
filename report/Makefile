PDFLATEX ?= pdflatex

all: report.pdf

report.pdf: report.tex
	@if which $(PDFLATEX) >/dev/null 2>&1; then \
		echo "Building with pdflatex..."; \
		$(PDFLATEX) -interaction=nonstopmode report.tex && $(PDFLATEX) -interaction=nonstopmode report.tex; \
	else \
		echo "pdflatex not found on host. Building publication PDF with generate_report_pdf.py..."; \
		python3 generate_report_pdf.py; \
	fi

clean:
	rm -f *.aux *.log *.out report.pdf

.PHONY: all clean
